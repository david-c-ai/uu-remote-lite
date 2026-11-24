import 'dart:async';
import 'dart:io';
import 'dart:ui' as ui;
import 'package:flutter/material.dart';
import 'ffi/uucore_bridge.dart';
import 'models/session_models.dart';
import 'widgets/file_transfer_sheet.dart';
import 'widgets/mobile_touch_overlay.dart';
import 'widgets/remote_canvas.dart';

void main() {
  WidgetsFlutterBinding.ensureInitialized();
  runApp(const UURemoteLiteApp());
}

class UURemoteLiteApp extends StatelessWidget {
  const UURemoteLiteApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'UU Remote Lite',
      debugShowCheckedModeBanner: false,
      theme: ThemeData.dark(useMaterial3: true).copyWith(
        colorScheme: ColorScheme.fromSeed(
          seedColor: Colors.tealAccent,
          brightness: Brightness.dark,
        ),
        scaffoldBackgroundColor: const Color(0xFF0F131A),
      ),
      home: const RemoteHomeScreen(),
    );
  }
}

class RemoteHomeScreen extends StatefulWidget {
  const RemoteHomeScreen({super.key});

  @override
  State<RemoteHomeScreen> createState() => _RemoteHomeScreenState();
}

class _RemoteHomeScreenState extends State<RemoteHomeScreen> {
  final _serverCtrl = TextEditingController(text: '127.0.0.1');
  final _selfIdCtrl = TextEditingController(text: '809120455');
  final _targetIdCtrl = TextEditingController(text: '100200300');
  final _pinCtrl = TextEditingController(text: '8866');

  UUCoreBridge? _bridge;
  bool _inSession = false;
  bool _hostServiceActive = false;
  bool _privacyCurtain = false;
  QualityPreset _preset = QualityPreset.balancedAuto;
  MobileInputMode _mobileMode = MobileInputMode.trackpadRelative;
  TelemetrySnapshot _telemetry = const TelemetrySnapshot();
  ui.Image? _frameImage;
  Offset _localCursor = const Offset(0.5, 0.5);
  Timer? _renderTimer;
  String _bannerMsg = '';

  bool get _isDesktop => Platform.isMacOS || Platform.isWindows || Platform.isLinux;

  @override
  void initState() {
    super.initState();
    _bridge = UUCoreBridge.tryLoad();
  }

  @override
  void dispose() {
    _renderTimer?.cancel();
    _bridge?.dispose();
    _serverCtrl.dispose();
    _selfIdCtrl.dispose();
    _targetIdCtrl.dispose();
    _pinCtrl.dispose();
    super.dispose();
  }

  void _toggleHostDaemon(bool enable) {
    final bridge = _bridge;
    if (bridge == null) {
      setState(() => _bannerMsg = '未检测到原生核心库 libuucore，请先运行 make 构建 core 模块');
      return;
    }
    if (enable) {
      final ok = bridge.startSession(
        selfId: _selfIdCtrl.text.trim(),
        accessPin: _pinCtrl.text.trim(),
        serverIp: _serverCtrl.text.trim(),
        isHost: true,
      );
      setState(() {
        _hostServiceActive = ok;
        _bannerMsg = ok ? '被控守护服务已就绪 (零轮询待机 <8MB)' : '启动被控服务失败';
      });
    } else {
      bridge.dispose();
      setState(() {
        _hostServiceActive = false;
        _bannerMsg = '被控服务已停止';
      });
    }
  }

  void _connectRemote() {
    final bridge = _bridge;
    if (bridge == null) {
      setState(() => _bannerMsg = '未加载 libuucore 动态库，请先执行根目录 make 编译核心库');
      return;
    }

    final started = bridge.startSession(
      selfId: _selfIdCtrl.text.trim(),
      accessPin: _pinCtrl.text.trim(),
      serverIp: _serverCtrl.text.trim(),
      isHost: false,
    );
    if (!started) {
      setState(() => _bannerMsg = '本地 UDP 传输套接字初始化失败');
      return;
    }

    final connected = bridge.connectToHost(_targetIdCtrl.text.trim(), timeoutMs: 2000);
    if (!connected) {
      setState(() => _bannerMsg = '连接超时：请确认远端设备在线且验证码一致');
      return;
    }

    setState(() {
      _inSession = true;
      _bannerMsg = '';
    });
    _startFramePump();
  }

  void _startFramePump() {
    _renderTimer?.cancel();
    _renderTimer = Timer.periodic(const Duration(milliseconds: 12), (_) {
      final bridge = _bridge;
      if (bridge == null || !_inSession) return;
      final tele = bridge.pollTelemetry();
      final frame = bridge.copyLatestFrame();
      if (frame != null && frame.width > 0 && frame.height > 0) {
        ui.decodeImageFromPixels(
          frame.bytes,
          frame.width,
          frame.height,
          ui.PixelFormat.bgra8888,
          (img) {
            if (!mounted) return;
            setState(() {
              _frameImage?.dispose();
              _frameImage = img;
              _telemetry = tele;
            });
          },
        );
      } else {
        setState(() => _telemetry = tele);
      }
    });
  }

  void _disconnectSession() {
    _renderTimer?.cancel();
    _bridge?.dispose();
    setState(() {
      _inSession = false;
      _frameImage?.dispose();
      _frameImage = null;
    });
  }

  @override
  Widget build(BuildContext context) {
    if (_inSession) {
      return _buildActiveSessionView();
    }
    return _buildConnectDashboard();
  }

  Widget _buildConnectDashboard() {
    return Scaffold(
      appBar: AppBar(
        title: const Text('UU Remote Lite · 极速窄带跨端远控'),
        backgroundColor: Colors.transparent,
        actions: [
          Padding(
            padding: const EdgeInsets.only(right: 16),
            child: Chip(
              avatar: const Icon(Icons.memory_rounded, size: 16),
              label: Text(_isDesktop ? 'Desktop (主/被控双模)' : 'Mobile App (控制端)'),
            ),
          ),
        ],
      ),
      body: Center(
        child: ConstrainedBox(
          constraints: const BoxConstraints(maxWidth: 760),
          child: ListView(
            padding: const EdgeInsets.all(24),
            children: [
              if (_bannerMsg.isNotEmpty)
                Container(
                  margin: const EdgeInsets.only(bottom: 16),
                  padding: const EdgeInsets.all(12),
                  decoration: BoxDecoration(
                    color: Colors.teal.withValues(alpha: 0.2),
                    borderRadius: BorderRadius.circular(10),
                  ),
                  child: Text(_bannerMsg),
                ),

              // Host Unattended Card (Windows / macOS)
              if (_isDesktop)
                Card(
                  color: const Color(0xFF171D28),
                  child: Padding(
                    padding: const EdgeInsets.all(20),
                    child: Column(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: [
                        Row(
                          mainAxisAlignment: MainAxisAlignment.spaceBetween,
                          children: [
                            const Text(
                              '本机被控端凭证 (允许手机 / 其他电脑远控本机)',
                              style: TextStyle(fontSize: 16, fontWeight: FontWeight.bold),
                            ),
                            Switch(
                              value: _hostServiceActive,
                              onChanged: _toggleHostDaemon,
                            ),
                          ],
                        ),
                        const SizedBox(height: 12),
                        Row(
                          children: [
                            Expanded(
                              child: TextField(
                                controller: _selfIdCtrl,
                                decoration: const InputDecoration(
                                  labelText: '本机设备 ID',
                                  border: OutlineInputBorder(),
                                ),
                              ),
                            ),
                            const SizedBox(width: 16),
                            Expanded(
                              child: TextField(
                                controller: _pinCtrl,
                                decoration: const InputDecoration(
                                  labelText: '连接验证码 (E2EE 派生密钥)',
                                  border: OutlineInputBorder(),
                                ),
                              ),
                            ),
                          ],
                        ),
                      ],
                    ),
                  ),
                ),

              const SizedBox(height: 16),

              // Remote Control Initiator Card (All 4 Platforms)
              Card(
                color: const Color(0xFF171D28),
                child: Padding(
                  padding: const EdgeInsets.all(20),
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.stretch,
                    children: [
                      const Text(
                        '发起极速远控 (Happy-Eyeballs P2P 打洞 + 中继秒连)',
                        style: TextStyle(fontSize: 16, fontWeight: FontWeight.bold),
                      ),
                      const SizedBox(height: 16),
                      TextField(
                        controller: _serverCtrl,
                        decoration: const InputDecoration(
                          labelText: '信令/中继服务器地址 (server-lite)',
                          prefixIcon: Icon(Icons.dns_rounded),
                          border: OutlineInputBorder(),
                        ),
                      ),
                      const SizedBox(height: 14),
                      Row(
                        children: [
                          Expanded(
                            flex: 2,
                            child: TextField(
                              controller: _targetIdCtrl,
                              decoration: const InputDecoration(
                                labelText: '伙伴设备 ID',
                                prefixIcon: Icon(Icons.desktop_windows_rounded),
                                border: OutlineInputBorder(),
                              ),
                            ),
                          ),
                          const SizedBox(width: 12),
                          Expanded(
                            child: TextField(
                              controller: _pinCtrl,
                              decoration: const InputDecoration(
                                labelText: '验证码',
                                prefixIcon: Icon(Icons.lock_outline_rounded),
                                border: OutlineInputBorder(),
                              ),
                            ),
                          ),
                        ],
                      ),
                      const SizedBox(height: 18),
                      FilledButton.icon(
                        style: FilledButton.styleFrom(
                          padding: const EdgeInsets.symmetric(vertical: 16),
                        ),
                        onPressed: _connectRemote,
                        icon: const Icon(Icons.flash_on_rounded),
                        label: const Text('立即秒连远端桌面', style: TextStyle(fontSize: 15)),
                      ),
                    ],
                  ),
                ),
              ),
            ],
          ),
        ),
      ),
    );
  }

  Widget _buildActiveSessionView() {
    return Scaffold(
      appBar: PreferredSize(
        preferredSize: const Size.fromHeight(46),
        child: AppBar(
          backgroundColor: const Color(0xFF141923),
          titleSpacing: 12,
          title: SingleChildScrollView(
            scrollDirection: Axis.horizontal,
            child: Row(
              children: [
                // Bandwidth & Quality Switcher
                DropdownButtonHideUnderline(
                  child: DropdownButton<QualityPreset>(
                    value: _preset,
                    dropdownColor: const Color(0xFF1C2331),
                    items: QualityPreset.values
                        .map((p) => DropdownMenuItem(
                              value: p,
                              child: Text(p.label, style: const TextStyle(fontSize: 13)),
                            ))
                        .toList(),
                    onChanged: (val) {
                      if (val == null) return;
                      setState(() => _preset = val);
                      _bridge?.setQuality(val, privacyCurtain: _privacyCurtain);
                    },
                  ),
                ),
                const SizedBox(width: 12),

                // Mobile Input Mode Selector
                SegmentedButton<MobileInputMode>(
                  segments: MobileInputMode.values
                      .map((m) => ButtonSegment(
                            value: m,
                            label: Text(m.label, style: const TextStyle(fontSize: 11)),
                          ))
                      .toList(),
                  selected: {_mobileMode},
                  onSelectionChanged: (s) => setState(() => _mobileMode = s.first),
                ),
                const SizedBox(width: 12),

                // Privacy Curtain Switch
                FilterChip(
                  selected: _privacyCurtain,
                  label: const Text('隐私屏防窥', style: TextStyle(fontSize: 12)),
                  onSelected: (v) {
                    setState(() => _privacyCurtain = v);
                    _bridge?.setQuality(_preset, privacyCurtain: v);
                  },
                ),
              ],
            ),
          ),
          actions: [
            IconButton(
              tooltip: '文件互传与剪贴板',
              icon: const Icon(Icons.folder_copy_rounded),
              onPressed: () {
                showModalBottomSheet(
                  context: context,
                  backgroundColor: const Color(0xFF171D28),
                  builder: (_) => FileTransferSheet(
                    onSendFilePath: (p) => _bridge?.sendFile(p) ?? false,
                    onSyncClipboardText: (t) => _bridge?.sendClipboard(t),
                  ),
                );
              },
            ),
            IconButton(
              tooltip: '断开连接',
              icon: const Icon(Icons.call_end_rounded, color: Colors.redAccent),
              onPressed: _disconnectSession,
            ),
          ],
        ),
      ),
      body: Stack(
        fit: StackFit.expand,
        children: [
          RemoteCanvas(
            frameImage: _frameImage,
            telemetry: _telemetry,
            localCursorNorm: _localCursor,
            onPointerEventNorm: (xn, yn, btnMask, wheelY) {
              setState(() => _localCursor = Offset(xn / 65535.0, yn / 65535.0));
              _bridge?.sendMouse(xn, yn, buttonMask: btnMask, wheelY: wheelY);
            },
          ),
          MobileTouchOverlay(
            mode: _mobileMode,
            onRelativeTrackpadDelta: (dx, dy) {
              final next = Offset(
                (_localCursor.dx + dx).clamp(0.0, 1.0),
                (_localCursor.dy + dy).clamp(0.0, 1.0),
              );
              setState(() => _localCursor = next);
              _bridge?.sendMouse(
                (next.dx * 65535).round(),
                (next.dy * 65535).round(),
                dx: (dx * 1920).round(),
                dy: (dy * 1080).round(),
              );
            },
            onTrackpadClick: (btnMask) {
              final xn = (_localCursor.dx * 65535).round();
              final yn = (_localCursor.dy * 65535).round();
              _bridge?.sendMouse(xn, yn, buttonMask: btnMask);
              Future.delayed(const Duration(milliseconds: 25), () {
                _bridge?.sendMouse(xn, yn, buttonMask: 0);
              });
            },
            onDirectTouchTapNorm: (xn, yn) {
              setState(() => _localCursor = Offset(xn / 65535.0, yn / 65535.0));
              _bridge?.sendMouse(xn, yn, buttonMask: 0x01);
              Future.delayed(const Duration(milliseconds: 25), () {
                _bridge?.sendMouse(xn, yn, buttonMask: 0);
              });
            },
            onShortcutKey: (keycode, mod) {
              _bridge?.sendKey(keycode, isDown: true, modifiers: mod);
              Future.delayed(const Duration(milliseconds: 25), () {
                _bridge?.sendKey(keycode, isDown: false, modifiers: 0);
              });
            },
            onGamepadUpdate: (buttons, lx, ly, rx, ry) {
              _bridge?.sendGamepad(buttons: buttons, lx: lx, ly: ly, rx: rx, ry: ry);
            },
          ),
        ],
      ),
    );
  }
}
