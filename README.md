# UU-Remote-Lite

面向 **Windows / macOS / iOS / Android** 的超轻量级、毫秒级响应、窄带弱网兼容、功能健全的远程控制底座实现（参考 `RustDesk` 的极简穿透/中继思想与 `UU远程` 的零延迟硬件流媒体及窄带混合管线）。

## 一、 核心指标实测数据（Apple Silicon Release `-O3 -flto`）

| 指标维度 | 实测结果 | 技术手段 |
| :--- | :--- | :--- |
| **服务端体积 (`uu-server-lite`)** | **`37 KB`**（单二进制含 `hbbs` 打洞 + `hbbr` 零拷贝中继） | 零外部重型库依赖、栈内存池原位切片转发 |
| **核心引擎库体积 (`libuucore.dylib`)** | **`155 KB`** | 原生系统硬编解码框架直调 + 内置轻量 X25519/ChaCha20 |
| **硬件编码 + 解码耗时 (720p HEVC)** | 编码 **`3.50 ms`** + 解码 **`1.33 ms`**（合计 **`4.83 ms`**） | Apple `VideoToolbox` 硬件 H.265（`RealTime=true`, `0 B-Frame`） |
| **低带宽兼容（代码编辑打字）** | 单帧增量仅 **`344 Bytes`**（20fps 仅需 **`55.0 Kbps`**，整链路含信令 **`147.9 Kbps`**） | 64x64 脏矩形检测 + **帧间 XOR 残差 + 硬件加速 LZ4**（像素级无损） |
| **静态桌面待机流量** | **`0 Kbps`** 视频码率 | 事件驱动 0-FPS 智能休眠 + 12 Bytes 独立光标通道分离 |
| **弱网抗丢包恢复 (10% UDP 丢包 + 对称 NAT)** | Happy-Eyeballs 自动中继兜底，0-RTT FEC 原地恢复 + 瞬时 PLI 关键帧自愈 | 自适应分组 XOR-FEC + 关键帧双倍冗余 + 40ms PLI 快速自愈 |

## 二、 目录结构

* `server-lite/`：极简 UDP ID 注册、NAT 打洞协调（默认 `:21116`）与零拷贝中继服务（默认 `:21117`）。
* `core/`：跨平台原生核心引擎（导出 `core/include/uucore_ffi.h` 标准 C ABI 供全平台调用）：
  * `crypto_e2ee`：RFC 7748 `X25519` 密钥协商 + `ChaCha20-AEAD` 端到端加密与防篡改校验。
  * `fec_qos`：`P0 (光标/鼠标/手柄) > P1 (音画/FEC) > P2 (键盘/剪贴板/文件)` 三级抢占调度器与窄带 AIMD 控制器。
  * `capture_hal`：macOS `ScreenCaptureKit` / Windows `DXGI` 屏幕采集、64x64 脏矩形追踪与光标哈希缓存分离。
  * `codec_hal`：硬件 H.265（`VideoToolbox`）与窄带 `Tile-XOR-Residual + LZ4` 双模智能切换编解码器。
  * `hid_clipboard_file`：跨平台键鼠/虚拟手柄注入（含断连防卡键释放）、LZ4 压缩剪贴板与带 `SHA-256` 校验的分片断点续传文件引擎。
* `client_flutter/`：统一支持 **Windows / macOS（主被控一体）** 与 **iOS / Android（控制端 App）** 的 Flutter 前端（含解耦本地硬件级光标渲染、移动端触控板加速度曲线、虚拟游戏手柄与悬浮组合键栏）。

## 三、 快速编译与运行验证

### 1. 编译原生服务端、核心库并运行五项端到端压测（macOS / Linux）
```bash
make test
```

### 2. 跨平台 CMake 构建（Windows MSVC / macOS / iOS / Android NDK）
```bash
mkdir -p cmake-build && cd cmake-build
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build . --config Release
```

### 3. Linux 云服务器一键启动信令与中继服务
直接从 GitHub Releases 一键下载并后台启动（零外部依赖静态二进制，兼容 Ubuntu / Debian / CentOS / Rocky 等全系 Linux）：
```bash
curl -sL https://github.com/david-c-ai/uu-remote-lite/releases/latest/download/UURemoteLite-Server-Linux-x64.tar.gz | tar -xz
cd uu-server-linux
./start.sh                 # 一键后台启动并自动打印服务器公网 IP
# 或安装为开机自启守护服务:
sudo ./install-systemd.sh
```
*(请在云服务器控制台安全组放行 **UDP `21116` 与 `21117`** 端口。)*

### 4. 全平台客户端安装包说明（GitHub Actions 自动产出）
每次推送代码或发布 `v*` 标签会自动构建全部 6 个平台产物：
* **`UURemoteLite-Server-Linux-x64.tar.gz`**：Linux 静态单文件服务端 + `start.sh` / `stop.sh` / `status.sh` / `install-systemd.sh`。
* **`UURemoteLite-macOS.dmg`**：macOS 主/被控客户端（首次打开前执行 `xattr -cr /Applications/UURemoteLite.app`）。
* **`UURemoteLite-Windows-x64.zip`**：Windows 便携免安装主/被控客户端 + `start-server.bat`。
* **`UURemoteLite-Android.apk`**：Android 手机/平板控制端。
* **`UURemoteLite-iOS-Unsigned.ipa`**：iPhone / iPad 控制端（内置 `VideoToolbox` H.265 硬解框架 `uucore.framework`，可通过 **Sideloadly / AltStore / 爱思助手** 用个人免费 Apple ID 自签安装）。
* **`UURemoteLite-Client-Linux-x64.tar.gz`**：Linux 桌面控制端。
