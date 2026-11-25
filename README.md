# DirRemote

面向 **Windows / macOS / iOS / Android / Linux** 的超轻量级、毫秒级响应、窄带弱网兼容且支持多显示器切换的跨平台远程控制系统。

## 一、 核心特性

* **极致轻量级**：服务端 `dir-server` 仅 ~37KB（Linux 纯静态包约 440KB），跨端核心引擎 `libdircore` 仅 ~155KB。
* **多屏无缝切换 & 移动端横竖屏沉浸体验**：自动枚举被控电脑的多台物理/扩展显示器，控制端支持一键切换显示器、横竖屏锁定/自动旋转、等比完整/全屏铺满及双指无极捏合缩放。
* **窄带与弱网兼容**：静态桌面 `0 Kbps`，代码打字增量约 `50~150 Kbps`；内置自适应 XOR-FEC 前向纠错与 40ms PLI 关键帧自愈。

## 二、 Linux 云服务器一键启动服务端

```bash
curl -sL https://github.com/david-c-ai/uu-remote-lite/releases/latest/download/DirRemote-Server-Linux-x64.tar.gz | tar -xz
cd dir-server-linux
./start.sh                 # 一键后台启动并打印公网 IP
# 或配置开机自启守护服务:
sudo ./install-systemd.sh
```
*(请在云服务器安全组放行 **UDP `21116` 与 `21117`** 端口。)*

## 三、 本地编译与验证

```bash
make clean && make test
```
