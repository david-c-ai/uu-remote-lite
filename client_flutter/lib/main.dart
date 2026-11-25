import 'dart:async';
import 'dart:io';
import 'dart:math' as math;
import 'dart:ui' as ui;
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'ffi/dircore_bridge.dart';
import 'models/session_models.dart';
import 'widgets/file_transfer_sheet.dart';
import 'widgets/mobile_touch_overlay.dart';
import 'widgets/remote_canvas.dart';

void main() {
  WidgetsFlutterBinding.ensureInitialized();
  runApp(const DirRemoteApp());
}

class DirRemoteApp extends StatelessWidget {
  const DirRemoteApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'DirRemote',
      debugShowCheckedModeBanner: false,
      theme: ThemeData.dark(useMaterial3: true).copyWith(
        colorScheme: ColorScheme.fromSeed(
          seedColor: const Color(0xFF38EF7D),
          brightness: Brightness.dark,
        ),
        scaffoldBackgroundColor: const Color(0xFF0B0F17),
      ),
      home: const DirHomeScreen(),
    );
  }
}

class DirHomeScreen extends StatefulWidget {
  const DirHomeScreen({super.key});

  @override
  State<DirHomeScreen> createState() => _DirHomeScreenState();
}

class _DirHomeScreenState extends State<DirHomeScreen> {
  final _serverCtrl = TextEditingController(text: '172.16.1.29');
  late final TextEditingController _selfIdCtrl;
  final _targetIdCtrl = TextEditingController(text: '100200300');
  final _pinCtrl = TextEditingController(text: '8866');

  DirCoreBridge? _bridge;
  bool _inSession = false;
  bool _connecting = false;
  bool _hostServiceActive = false;
  bool _privacyCurtain = false;
  bool _audioEnabled = true;
  bool _keyboardVisible = false;
  bool _topBarCollapsed = false;
  int _selectedMonitor = 0;
  int _clickRippleSeq = 0;

  QualityPreset _preset = QualityPreset.balancedAuto;
  CodecPreference _codecPref = CodecPreference.auto;
  MobileInputMode _mobileMode = MobileInputMode.trackpadRelative;
  TrackpadSensitivity _sensitivity = TrackpadSensitivity.standard;
  ScreenOrientationMode _orientationMode = ScreenOrientationMode.autoRotate;
  ViewportScaleMode _scaleMode = ViewportScaleMode.contain;

  TelemetrySnapshot _telemetry = const TelemetrySnapshot();
  ui.Image? _frameImage;
  Offset _localCursor = const Offset(0.5, 0.5);
  Timer? _renderTimer;
  String _bannerMsg = '';

  bool get _isDesktop => Platform.isMacOS || Platform.isWindows || Platform.isLinux;

  @override
  void initState() {
    super.initState();
    final defaultSelfId = _isDesktop
        ? '100200300'
        : '9${(10000000 + math.Random().nextInt(89999999))}';
    _selfIdCtrl = TextEditingController(text: defaultSelfId);
    _bridge = DirCoreBridge.tryLoad();
  }

  @override
  void dispose() {
    _restoreSystemOrientation();
    _renderTimer?.cancel();
    _bridge?.dispose();
    _serverCtrl.dispose();
    _selfIdCtrl.dispose();
    _targetIdCtrl.dispose();
    _pinCtrl.dispose();
    super.dispose();
  }

  Future<void> _applyOrientation(ScreenOrientationMode mode) async {
    setState(() => _orientationMode = mode);
    if (_isDesktop) return;

    switch (mode) {
      case ScreenOrientationMode.landscape:
        await SystemChrome.setEnabledSystemUIMode(SystemUiMode.immersiveSticky);
        await SystemChrome.setPreferredOrientations([
          DeviceOrientation.landscapeLeft,
          DeviceOrientation.landscapeRight,
        ]);
        break;
      case ScreenOrientationMode.portrait:
        await SystemChrome.setEnabledSystemUIMode(SystemUiMode.edgeToEdge);
        await SystemChrome.setPreferredOrientations([
          DeviceOrientation.portraitUp,
        ]);
        break;
      case ScreenOrientationMode.autoRotate:
        await SystemChrome.setEnabledSystemUIMode(SystemUiMode.immersiveSticky);
        await SystemChrome.setPreferredOrientations(DeviceOrientation.values);
        break;
    }
  }

  Future<void> _cycleOrientation() async {
    final next = switch (_orientationMode) {
      ScreenOrientationMode.autoRotate => ScreenOrientationMode.landscape,
      ScreenOrientationMode.landscape => ScreenOrientationMode.portrait,
      ScreenOrientationMode.portrait => ScreenOrientationMode.autoRotate,
    };
    await _applyOrientation(next);
  }

  Future<void> _restoreSystemOrientation() async {
    if (_isDesktop) return;
    await SystemChrome.setEnabledSystemUIMode(SystemUiMode.edgeToEdge);
    await SystemChrome.setPreferredOrientations(DeviceOrientation.values);
  }

  void _toggleHostDaemon(bool enable) {
    final bridge = _bridge;
    if (bridge == null) {
      setState(() => _bannerMsg = '未加载核心引擎 libdircore，请确认动态库在程序目录');
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
        _bannerMsg = ok ? 'DirRemote 被控服务已上线 (${_serverCtrl.text.trim()})' : '启动被控服务失败';
      });
    } else {
      bridge.dispose();
      setState(() {
        _hostServiceActive = false;
        _bannerMsg = '被控服务已停止';
      });
    }
  }

  Future<void> _connectRemote() async {
    final bridge = _bridge;
    if (bridge == null) {
      setState(() => _bannerMsg = '未加载 libdircore 原生引擎');
      return;
    }

    setState(() {
      _connecting = true;
      _bannerMsg = '';
    });

    await Future<void>.delayed(const Duration(milliseconds: 20));

    final started = bridge.startSession(
      selfId: _selfIdCtrl.text.trim(),
      accessPin: _pinCtrl.text.trim(),
      serverIp: _serverCtrl.text.trim(),
      isHost: false,
    );
    if (!started) {
      setState(() {
        _connecting = false;
        _bannerMsg = '本地网络端口初始化失败';
      });
      return;
    }

    final connected = bridge.connectToHost(_targetIdCtrl.text.trim(), timeoutMs: 2500);
    if (!connected) {
      setState(() {
        _connecting = false;
        _bannerMsg = '连接超时：请确认被控端开关已开启、服务器IP及验证码一致';
      });
      return;
    }

    setState(() {
      _connecting = false;
      _inSession = true;
      _selectedMonitor = 0;
      _bannerMsg = '';
    });

    if (!_isDesktop) {
      await SystemChrome.setEnabledSystemUIMode(SystemUiMode.immersiveSticky);
    }
    _startFramePump();
  }

  bool _decodingFrame = false;
  int _lastFrameSeq = 0;

  void _startFramePump() {
    _renderTimer?.cancel();
    _lastFrameSeq = 0;
    _renderTimer = Timer.periodic(const Duration(milliseconds: 16), (_) async {
      final bridge = _bridge;
      if (bridge == null || !_inSession || _decodingFrame) return;

      final tele = bridge.pollTelemetry();
      final frame = bridge.copyLatestFrameIfNewer(_lastFrameSeq);
      if (frame == null) {
        if (mounted) {
          setState(() {
            _telemetry = tele;
            _selectedMonitor = tele.activeMonitor;
          });
        }
        return;
      }

      _decodingFrame = true;
      _lastFrameSeq = frame.seq;
      if (frame.format == 2) {
        // Hardware/SIMD libjpeg-turbo decoding via Flutter's native image codec (~1.5ms!)
        try {
          final codec = await ui.instantiateImageCodec(frame.bytes);
          final info = await codec.getNextFrame();
          codec.dispose();
          _decodingFrame = false;
          if (!mounted || !_inSession) {
            info.image.dispose();
            return;
          }
          setState(() {
            _frameImage?.dispose();
            _frameImage = info.image;
            _telemetry = tele;
            _selectedMonitor = tele.activeMonitor;
          });
        } catch (_) {
          _decodingFrame = false;
        }
      } else if (frame.width > 0 && frame.height > 0) {
        ui.decodeImageFromPixels(
          frame.bytes,
          frame.width,
          frame.height,
          ui.PixelFormat.rgba8888,
          (img) {
            _decodingFrame = false;
            if (!mounted || !_inSession) {
              img.dispose();
              return;
            }
            setState(() {
              _frameImage?.dispose();
              _frameImage = img;
              _telemetry = tele;
              _selectedMonitor = tele.activeMonitor;
            });
          },
          rowBytes: frame.width * 4,
        );
      } else {
        _decodingFrame = false;
      }
    });
  }

  void _pushSessionOptions() {
    _bridge?.setOptions(
      preset: _preset,
      privacyCurtain: _privacyCurtain,
      monitorIndex: _selectedMonitor,
      codecPreference: _codecPref,
      audioEnabled: _audioEnabled,
    );
  }

  void _switchMonitor(int nextMonitor) {
    setState(() => _selectedMonitor = nextMonitor);
    _pushSessionOptions();
  }

  Future<void> _disconnectSession() async {
    _renderTimer?.cancel();
    _bridge?.dispose();
    await _restoreSystemOrientation();
    if (!mounted) return;
    setState(() {
      _inSession = false;
      _keyboardVisible = false;
      _topBarCollapsed = false;
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
    final isLandscape = MediaQuery.of(context).orientation == Orientation.landscape;

    return Scaffold(
      body: SafeArea(
        child: Center(
          child: ConstrainedBox(
            constraints: const BoxConstraints(maxWidth: 820),
            child: ListView(
              padding: EdgeInsets.symmetric(
                horizontal: isLandscape ? 28 : 18,
                vertical: 18,
              ),
              children: [
                // Brand Header Card
                Container(
                  padding: const EdgeInsets.all(20),
                  decoration: BoxDecoration(
                    gradient: const LinearGradient(
                      colors: [Color(0xFF132A2E), Color(0xFF141B29)],
                      begin: Alignment.topLeft,
                      end: Alignment.bottomRight,
                    ),
                    borderRadius: BorderRadius.circular(20),
                    border: Border.all(color: Colors.white12),
                  ),
                  child: Row(
                    children: [
                      Container(
                        width: 46,
                        height: 46,
                        decoration: BoxDecoration(
                          color: const Color(0xFF38EF7D).withValues(alpha: 0.16),
                          borderRadius: BorderRadius.circular(14),
                        ),
                        child: const Icon(
                          Icons.bolt_rounded,
                          color: Color(0xFF38EF7D),
                          size: 28,
                        ),
                      ),
                      const SizedBox(width: 14),
                      Expanded(
                        child: Column(
                          crossAxisAlignment: CrossAxisAlignment.start,
                          children: [
                            const Text(
                              'DirRemote',
                              style: TextStyle(
                                fontSize: 21,
                                fontWeight: FontWeight.w800,
                                letterSpacing: 0.4,
                              ),
                            ),
                            const SizedBox(height: 3),
                            Text(
                              _isDesktop
                                  ? '桌面主/被控一体端 · 零拷贝窄带引擎 · 多屏自适应'
                                  : '移动触控远控端 · 支持横竖屏切换 · 多显示器秒切',
                              style: const TextStyle(color: Colors.white60, fontSize: 12.5),
                            ),
                          ],
                        ),
                      ),
                      if (!_isDesktop)
                        IconButton.filledTonal(
                          tooltip: '切换横竖屏 (${_orientationMode.label})',
                          onPressed: _cycleOrientation,
                          icon: const Icon(Icons.screen_rotation_rounded, size: 20),
                        ),
                    ],
                  ),
                ),

                if (_bannerMsg.isNotEmpty)
                  Container(
                    margin: const EdgeInsets.only(top: 14),
                    padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 11),
                    decoration: BoxDecoration(
                      color: const Color(0xFF1E3A34),
                      borderRadius: BorderRadius.circular(12),
                      border: Border.all(color: const Color(0xFF38EF7D).withValues(alpha: 0.35)),
                    ),
                    child: Text(_bannerMsg, style: const TextStyle(fontSize: 13)),
                  ),

                const SizedBox(height: 16),

                // Desktop Host Unattended Card
                if (_isDesktop) ...[
                  _sectionCard(
                    title: '允许远控本机 (被控服务)',
                    trailing: Switch(
                      value: _hostServiceActive,
                      onChanged: _toggleHostDaemon,
                    ),
                    child: Column(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: [
                        Row(
                          children: [
                            Expanded(
                              child: TextField(
                                controller: _selfIdCtrl,
                                decoration: const InputDecoration(
                                  labelText: '本机设备 ID',
                                  prefixIcon: Icon(Icons.tag_rounded, size: 19),
                                  border: OutlineInputBorder(),
                                  isDense: true,
                                ),
                              ),
                            ),
                            const SizedBox(width: 12),
                            Expanded(
                              child: TextField(
                                controller: _pinCtrl,
                                decoration: const InputDecoration(
                                  labelText: '连接验证码',
                                  prefixIcon: Icon(Icons.key_rounded, size: 19),
                                  border: OutlineInputBorder(),
                                  isDense: true,
                                ),
                              ),
                            ),
                          ],
                        ),
                        if (Platform.isMacOS) ...[
                          const SizedBox(height: 10),
                          const Text(
                            '提示：macOS 覆盖安装新版本后若提示权限或无画面，请在「系统设置 → 隐私与安全性 → 屏幕与系统音频录制 / 辅助功能」中先关闭再重新勾选 DirRemote 并重启 App。',
                            style: TextStyle(color: Colors.amberAccent, fontSize: 11.5, height: 1.35),
                          ),
                        ],
                      ],
                    ),
                  ),
                  const SizedBox(height: 16),
                ],

                // Remote Connect Card
                _sectionCard(
                  title: '连接远端电脑',
                  trailing: Text(
                    '本机ID: ${_selfIdCtrl.text}',
                    style: const TextStyle(color: Colors.white38, fontSize: 11.5),
                  ),
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.stretch,
                    children: [
                      TextField(
                        controller: _serverCtrl,
                        decoration: const InputDecoration(
                          labelText: '信令 / 中继服务器 IP (dir-server)',
                          prefixIcon: Icon(Icons.dns_rounded, size: 19),
                          border: OutlineInputBorder(),
                          isDense: true,
                        ),
                      ),
                      const SizedBox(height: 12),
                      Row(
                        children: [
                          Expanded(
                            flex: 3,
                            child: TextField(
                              controller: _targetIdCtrl,
                              keyboardType: TextInputType.number,
                              decoration: const InputDecoration(
                                labelText: '目标设备 ID',
                                prefixIcon: Icon(Icons.desktop_mac_rounded, size: 19),
                                border: OutlineInputBorder(),
                                isDense: true,
                              ),
                            ),
                          ),
                          const SizedBox(width: 10),
                          Expanded(
                            flex: 2,
                            child: TextField(
                              controller: _pinCtrl,
                              keyboardType: TextInputType.visiblePassword,
                              decoration: const InputDecoration(
                                labelText: '验证码',
                                prefixIcon: Icon(Icons.lock_outline_rounded, size: 19),
                                border: OutlineInputBorder(),
                                isDense: true,
                              ),
                            ),
                          ),
                        ],
                      ),
                      const SizedBox(height: 16),
                      FilledButton.icon(
                        style: FilledButton.styleFrom(
                          backgroundColor: const Color(0xFF2BD973),
                          foregroundColor: Colors.black,
                          padding: const EdgeInsets.symmetric(vertical: 15),
                          shape: RoundedRectangleBorder(
                            borderRadius: BorderRadius.circular(14),
                          ),
                        ),
                        onPressed: _connecting ? null : _connectRemote,
                        icon: _connecting
                            ? const SizedBox(
                                width: 18,
                                height: 18,
                                child: CircularProgressIndicator(strokeWidth: 2, color: Colors.black),
                              )
                            : const Icon(Icons.flash_on_rounded),
                        label: Text(
                          _connecting ? '正在握手打洞...' : '立即极速连接',
                          style: const TextStyle(fontSize: 15, fontWeight: FontWeight.w700),
                        ),
                      ),
                    ],
                  ),
                ),
              ],
            ),
          ),
        ),
      ),
    );
  }

  Widget _sectionCard({
    required String title,
    Widget? trailing,
    required Widget child,
  }) {
    return Container(
      padding: const EdgeInsets.all(18),
      decoration: BoxDecoration(
        color: const Color(0xFF141A26),
        borderRadius: BorderRadius.circular(18),
        border: Border.all(color: Colors.white10),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          Row(
            mainAxisAlignment: MainAxisAlignment.spaceBetween,
            children: [
              Text(
                title,
                style: const TextStyle(fontSize: 15.5, fontWeight: FontWeight.w700),
              ),
              if (trailing != null) trailing,
            ],
          ),
          const SizedBox(height: 14),
          child,
        ],
      ),
    );
  }

  Widget _buildActiveSessionView() {
    final monCount = math.max(1, _telemetry.monitorCount);

    return Scaffold(
      backgroundColor: const Color(0xFF0A0D12),
      resizeToAvoidBottomInset: false,
      body: Stack(
        fit: StackFit.expand,
        children: [
          // 1. Remote Desktop Canvas + Precision 1px-Hotspot Cursor + Zoom-Aware Tap + Desktop Physical Keyboard
          RemoteCanvas(
            frameImage: _frameImage,
            telemetry: _telemetry,
            localCursorNorm: _localCursor,
            scaleMode: _scaleMode,
            enablePinchZoom: !_isDesktop && _mobileMode == MobileInputMode.directTouchZoom,
            capturePhysicalKeyboard: _isDesktop && !_keyboardVisible,
            clickRippleSeq: _clickRippleSeq,
            onPointerEventNorm: (xn, yn, btnMask, wheelY) {
              setState(() {
                _localCursor = Offset(xn / 65535.0, yn / 65535.0);
                if (btnMask != 0) _clickRippleSeq++;
              });
              _bridge?.sendMouse(xn, yn, buttonMask: btnMask, wheelY: wheelY);
            },
            onDirectTouchTapNorm: (xn, yn) {
              setState(() {
                _localCursor = Offset(xn / 65535.0, yn / 65535.0);
                _clickRippleSeq++;
              });
              _bridge?.sendMouse(xn, yn, buttonMask: 0x01);
              Future.delayed(const Duration(milliseconds: 28), () {
                _bridge?.sendMouse(xn, yn, buttonMask: 0);
              });
            },
            onPhysicalKey: (dirKeyCode, isDown, modifiers) {
              _bridge?.sendKey(dirKeyCode, isDown: isDown, modifiers: modifiers);
            },
          ),

          // 2. Mobile Gesture, Virtual Keyboard, Dual-Stick Gamepad & Quick-Action Dock
          MobileTouchOverlay(
            isDesktop: _isDesktop,
            mode: _mobileMode,
            sensitivity: _sensitivity,
            keyboardVisible: _keyboardVisible,
            onToggleKeyboard: () => setState(() => _keyboardVisible = !_keyboardVisible),
            onCycleSensitivity: () {
              final next = switch (_sensitivity) {
                TrackpadSensitivity.precise => TrackpadSensitivity.standard,
                TrackpadSensitivity.standard => TrackpadSensitivity.fast,
                TrackpadSensitivity.fast => TrackpadSensitivity.precise,
              };
              setState(() => _sensitivity = next);
            },
            onRelativeTrackpadDelta: (dx, dy) {
              final next = Offset(
                (_localCursor.dx + dx).clamp(0.0, 1.0),
                (_localCursor.dy + dy).clamp(0.0, 1.0),
              );
              setState(() => _localCursor = next);
              // Send exact normalized absolute coordinate (dx=0, dy=0) so cursor matches 1:1 with zero drift!
              _bridge?.sendMouse(
                (next.dx * 65535).round(),
                (next.dy * 65535).round(),
              );
            },
            onTrackpadClick: (btnMask) {
              final xn = (_localCursor.dx * 65535).round();
              final yn = (_localCursor.dy * 65535).round();
              setState(() => _clickRippleSeq++);
              _bridge?.sendMouse(xn, yn, buttonMask: btnMask);
              Future.delayed(const Duration(milliseconds: 28), () {
                _bridge?.sendMouse(xn, yn, buttonMask: 0);
              });
            },
            onTrackpadScroll: (wheelDelta) {
              final xn = (_localCursor.dx * 65535).round();
              final yn = (_localCursor.dy * 65535).round();
              _bridge?.sendMouse(xn, yn, wheelY: wheelDelta);
            },
            onShortcutKey: (keycode, mod) {
              _bridge?.sendKey(keycode, isDown: true, modifiers: mod);
              Future.delayed(const Duration(milliseconds: 28), () {
                _bridge?.sendKey(keycode, isDown: false, modifiers: 0);
              });
            },
            onSendUnicodeText: (utf8Text) {
              _bridge?.sendText(utf8Text);
            },
            onGamepadUpdate: (buttons, lx, ly, rx, ry, lt, rt) {
              _bridge?.sendGamepad(
                buttons: buttons,
                lx: lx,
                ly: ly,
                rx: rx,
                ry: ry,
                lt: lt,
                rt: rt,
              );
            },
          ),

          // 3. Top-Right Collapsible Floating Session Toolbar (Supports Portrait & Landscape + Multi-Monitor!)
          Positioned(
            top: 8,
            right: 12,
            left: 12,
            child: SafeArea(
              bottom: false,
              child: Align(
                alignment: Alignment.topRight,
                child: _topBarCollapsed
                    ? IconButton.filledTonal(
                        style: IconButton.styleFrom(
                          backgroundColor: const Color(0xCC151C28),
                        ),
                        tooltip: '展开控制栏',
                        icon: const Icon(Icons.tune_rounded, size: 18),
                        onPressed: () => setState(() => _topBarCollapsed = false),
                      )
                    : SingleChildScrollView(
                        scrollDirection: Axis.horizontal,
                        child: Container(
                          padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 4),
                          decoration: BoxDecoration(
                            color: const Color(0xE0131924),
                            borderRadius: BorderRadius.circular(22),
                            border: Border.all(color: Colors.white12),
                          ),
                          child: Row(
                            mainAxisSize: MainAxisSize.min,
                            children: [
                              // Multi-Monitor Switcher (Cycles displays when host has multiple screens, or lets user pick)
                              PopupMenuButton<int>(
                                tooltip: '切换被控端显示器',
                                color: const Color(0xFF1A2232),
                                onSelected: _switchMonitor,
                                itemBuilder: (_) {
                                  final count = math.max(monCount, 2);
                                  return List.generate(
                                    count,
                                    (idx) => PopupMenuItem<int>(
                                      value: idx,
                                      child: Row(
                                        children: [
                                          Icon(
                                            Icons.monitor_rounded,
                                            size: 16,
                                            color: idx == _selectedMonitor
                                                ? const Color(0xFF38EF7D)
                                                : Colors.white54,
                                          ),
                                          const SizedBox(width: 8),
                                          Text('显示器 ${idx + 1}${idx >= monCount ? " (扩展)" : ""}'),
                                        ],
                                      ),
                                    ),
                                  );
                                },
                                child: Padding(
                                  padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 5),
                                  child: Row(
                                    mainAxisSize: MainAxisSize.min,
                                    children: [
                                      const Icon(Icons.monitor_rounded, size: 15, color: Color(0xFF38EF7D)),
                                      const SizedBox(width: 4),
                                      Text(
                                        '屏幕 ${_selectedMonitor + 1}/$monCount',
                                        style: const TextStyle(fontSize: 12, fontWeight: FontWeight.w600),
                                      ),
                                    ],
                                  ),
                                ),
                              ),

                              // Orientation Toggle (Portrait / Landscape / Auto) on Mobile
                              if (!_isDesktop)
                                _barBtn(
                                  icon: Icons.screen_rotation_rounded,
                                  label: _orientationMode == ScreenOrientationMode.landscape
                                      ? '竖屏'
                                      : (_orientationMode == ScreenOrientationMode.portrait ? '自动' : '横屏'),
                                  onTap: _cycleOrientation,
                                ),

                              // Aspect Ratio Fit Toggle (Contain vs Fill)
                              _barBtn(
                                icon: Icons.aspect_ratio_rounded,
                                label: _scaleMode.label,
                                onTap: () {
                                  setState(() {
                                    _scaleMode = _scaleMode == ViewportScaleMode.contain
                                        ? ViewportScaleMode.fill
                                        : ViewportScaleMode.contain;
                                  });
                                },
                              ),

                              // Input Mode Cycle (Trackpad / Direct Zoom / Gamepad)
                              _barBtn(
                                icon: Icons.mouse_rounded,
                                label: _mobileMode.label,
                                onTap: () {
                                  final next = switch (_mobileMode) {
                                    MobileInputMode.trackpadRelative => MobileInputMode.directTouchZoom,
                                    MobileInputMode.directTouchZoom => MobileInputMode.virtualGamepad,
                                    MobileInputMode.virtualGamepad => MobileInputMode.trackpadRelative,
                                  };
                                  setState(() => _mobileMode = next);
                                },
                              ),

                              // System Audio Toggle (Mute / Unmute)
                              IconButton(
                                visualDensity: VisualDensity.compact,
                                tooltip: _audioEnabled ? '关闭远端声音' : '开启远端声音',
                                icon: Icon(
                                  _audioEnabled ? Icons.volume_up_rounded : Icons.volume_off_rounded,
                                  size: 17,
                                  color: _audioEnabled ? const Color(0xFF38EF7D) : Colors.white38,
                                ),
                                onPressed: () {
                                  setState(() => _audioEnabled = !_audioEnabled);
                                  _pushSessionOptions();
                                },
                              ),

                              // Virtual Keyboard Toggle (Mobile IME & Special Keys)
                              IconButton(
                                visualDensity: VisualDensity.compact,
                                tooltip: _keyboardVisible ? '收起虚拟键盘' : '打开虚拟键盘',
                                icon: Icon(
                                  Icons.keyboard_alt_rounded,
                                  size: 17,
                                  color: _keyboardVisible ? const Color(0xFF38EF7D) : Colors.white70,
                                ),
                                onPressed: () => setState(() => _keyboardVisible = !_keyboardVisible),
                              ),

                              // GPU Hardware Codec Selector
                              PopupMenuButton<CodecPreference>(
                                tooltip: '选择编解码方式 (GPU 硬解 / JPEG / 无损)',
                                color: const Color(0xFF1A2232),
                                onSelected: (val) {
                                  setState(() => _codecPref = val);
                                  _pushSessionOptions();
                                },
                                itemBuilder: (_) => CodecPreference.values
                                    .map((c) => PopupMenuItem(
                                          value: c,
                                          child: Column(
                                            crossAxisAlignment: CrossAxisAlignment.start,
                                            mainAxisSize: MainAxisSize.min,
                                            children: [
                                              Text(
                                                c.shortLabel,
                                                style: TextStyle(
                                                  fontWeight: FontWeight.w600,
                                                  color: c == _codecPref
                                                      ? const Color(0xFF38EF7D)
                                                      : Colors.white,
                                                ),
                                              ),
                                              Text(
                                                c.subtitle,
                                                style: const TextStyle(fontSize: 11, color: Colors.white54),
                                              ),
                                            ],
                                          ),
                                        ))
                                    .toList(),
                                child: Padding(
                                  padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 5),
                                  child: Row(
                                    mainAxisSize: MainAxisSize.min,
                                    children: [
                                      const Icon(Icons.memory_rounded, size: 14, color: Colors.lightBlueAccent),
                                      const SizedBox(width: 4),
                                      Text(
                                        _codecPref.shortLabel,
                                        style: const TextStyle(fontSize: 12, color: Colors.lightBlueAccent),
                                      ),
                                    ],
                                  ),
                                ),
                              ),

                              // Bandwidth / Quality Mode Menu
                              PopupMenuButton<QualityPreset>(
                                tooltip: '画质与带宽模式',
                                color: const Color(0xFF1A2232),
                                onSelected: (val) {
                                  setState(() => _preset = val);
                                  _pushSessionOptions();
                                },
                                itemBuilder: (_) => QualityPreset.values
                                    .map((p) => PopupMenuItem(
                                          value: p,
                                          child: Text('${p.shortLabel} (${p.subtitle})'),
                                        ))
                                    .toList(),
                                child: Padding(
                                  padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 5),
                                  child: Text(
                                    _preset.shortLabel,
                                    style: const TextStyle(fontSize: 12, color: Colors.tealAccent),
                                  ),
                                ),
                              ),

                              // File & Clipboard Sheet
                              IconButton(
                                visualDensity: VisualDensity.compact,
                                tooltip: '文件传输与剪贴板',
                                icon: const Icon(Icons.folder_copy_rounded, size: 17),
                                onPressed: () {
                                  showModalBottomSheet(
                                    context: context,
                                    backgroundColor: const Color(0xFF141A26),
                                    builder: (_) => FileTransferSheet(
                                      onSendFilePath: (p) => _bridge?.sendFile(p) ?? false,
                                      onSyncClipboardText: (t) => _bridge?.sendClipboard(t),
                                    ),
                                  );
                                },
                              ),

                              // Collapse Toolbar Button
                              IconButton(
                                visualDensity: VisualDensity.compact,
                                tooltip: '收起顶栏',
                                icon: const Icon(Icons.chevron_right_rounded, size: 19, color: Colors.white54),
                                onPressed: () => setState(() => _topBarCollapsed = true),
                              ),

                              // Disconnect Button
                              IconButton(
                                visualDensity: VisualDensity.compact,
                                tooltip: '断开远控',
                                icon: const Icon(Icons.power_settings_new_rounded,
                                    size: 18, color: Colors.redAccent),
                                onPressed: _disconnectSession,
                              ),
                            ],
                          ),
                        ),
                      ),
              ),
            ),
          ),
        ],
      ),
    );
  }

  Widget _barBtn({
    required IconData icon,
    required String label,
    required VoidCallback onTap,
  }) {
    return InkWell(
      onTap: onTap,
      borderRadius: BorderRadius.circular(12),
      child: Padding(
        padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 5),
        child: Row(
          mainAxisSize: MainAxisSize.min,
          children: [
            Icon(icon, size: 14, color: Colors.white70),
            const SizedBox(width: 4),
            Text(label, style: const TextStyle(fontSize: 11.5, color: Colors.white)),
          ],
        ),
      ),
    );
  }
}
