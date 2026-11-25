import 'dart:async';
import 'dart:math' as math;
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import '../models/session_models.dart';

/// Canonical USB HID DirKey codes matching core/include/keymap_hal.hpp
class DirKeyCodes {
  static const int a = 0x04;
  static const int c = 0x06;
  static const int v = 0x19;
  static const int z = 0x1D;
  static const int enter = 0x28;
  static const int escape = 0x29;
  static const int backspace = 0x2A;
  static const int tab = 0x2B;
  static const int space = 0x2C;
  static const int delete = 0x4C;
  static const int right = 0x4F;
  static const int left = 0x50;
  static const int down = 0x51;
  static const int up = 0x52;
  static const int leftMeta = 0xE3;
}

class MobileTouchOverlay extends StatefulWidget {
  const MobileTouchOverlay({
    super.key,
    required this.isDesktop,
    required this.mode,
    required this.sensitivity,
    required this.keyboardVisible,
    required this.onToggleKeyboard,
    required this.onCycleSensitivity,
    required this.onRelativeTrackpadDelta,
    required this.onTrackpadClick,
    required this.onTrackpadScroll,
    required this.onShortcutKey,
    required this.onSendUnicodeText,
    required this.onGamepadUpdate,
  });

  final bool isDesktop;
  final MobileInputMode mode;
  final TrackpadSensitivity sensitivity;
  final bool keyboardVisible;
  final VoidCallback onToggleKeyboard;
  final VoidCallback onCycleSensitivity;
  final void Function(double dxNorm, double dyNorm) onRelativeTrackpadDelta;
  final void Function(int buttonMask) onTrackpadClick;
  final void Function(int wheelDelta) onTrackpadScroll;
  final void Function(int dirKeyCode, int modifiers) onShortcutKey;
  final void Function(String utf8Text) onSendUnicodeText;
  final void Function(int buttons, int lx, int ly, int rx, int ry, int lt, int rt) onGamepadUpdate;

  @override
  State<MobileTouchOverlay> createState() => _MobileTouchOverlayState();
}

class _MobileTouchOverlayState extends State<MobileTouchOverlay> {
  int _buttonsMask = 0;
  int _ltVal = 0;
  int _rtVal = 0;
  Offset _leftStick = Offset.zero;
  Offset _rightStick = Offset.zero;
  Timer? _gamepadTick;

  bool _dockCollapsed = false;
  int _stickyMod = 0; // Bit 1: Ctrl, Bit 2: Alt, Bit 3: Cmd/Win
  final TextEditingController _imeCtrl = TextEditingController();
  final FocusNode _imeFocus = FocusNode();

  @override
  void didUpdateWidget(covariant MobileTouchOverlay oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (widget.keyboardVisible && !oldWidget.keyboardVisible) {
      WidgetsBinding.instance.addPostFrameCallback((_) {
        if (mounted) _imeFocus.requestFocus();
      });
    } else if (!widget.keyboardVisible && oldWidget.keyboardVisible) {
      _imeFocus.unfocus();
    }
    if (oldWidget.mode == MobileInputMode.virtualGamepad &&
        widget.mode != MobileInputMode.virtualGamepad) {
      _resetGamepad();
    }
  }

  @override
  void dispose() {
    _gamepadTick?.cancel();
    _imeCtrl.dispose();
    _imeFocus.dispose();
    super.dispose();
  }

  void _resetGamepad() {
    _gamepadTick?.cancel();
    _gamepadTick = null;
    _buttonsMask = 0;
    _ltVal = 0;
    _rtVal = 0;
    _leftStick = Offset.zero;
    _rightStick = Offset.zero;
    widget.onGamepadUpdate(0, 0, 0, 0, 0, 0, 0);
  }

  void _emitGamepad() {
    widget.onGamepadUpdate(
      _buttonsMask,
      (_leftStick.dx * 32767).round(),
      (_leftStick.dy * 32767).round(),
      (_rightStick.dx * 32767).round(),
      (_rightStick.dy * 32767).round(),
      _ltVal,
      _rtVal,
    );
  }

  void _syncRightStickLoop() {
    final active = _rightStick.distance > 0.14;
    if (active && _gamepadTick == null) {
      _gamepadTick = Timer.periodic(const Duration(milliseconds: 33), (_) => _emitGamepad());
    } else if (!active && _gamepadTick != null) {
      _gamepadTick?.cancel();
      _gamepadTick = null;
    }
  }

  void _toggleButton(int bit, bool pressed) {
    if (pressed) HapticFeedback.selectionClick();
    setState(() {
      if (pressed) {
        _buttonsMask |= bit;
      } else {
        _buttonsMask &= ~bit;
      }
    });
    _emitGamepad();
  }

  void _toggleTrigger({required bool isLeft, required bool pressed}) {
    if (pressed) HapticFeedback.lightImpact();
    setState(() {
      if (isLeft) {
        _ltVal = pressed ? 255 : 0;
      } else {
        _rtVal = pressed ? 255 : 0;
      }
    });
    _emitGamepad();
  }

  void _sendImeBuffer() {
    final txt = _imeCtrl.text;
    if (txt.isEmpty) {
      widget.onShortcutKey(DirKeyCodes.enter, _stickyMod);
      return;
    }
    // If single ASCII letter with sticky modifier (e.g. Ctrl+S), send key chord instead of text
    if (_stickyMod != 0 && txt.length == 1) {
      final codeUnit = txt.toLowerCase().codeUnitAt(0);
      if (codeUnit >= 97 && codeUnit <= 122) {
        widget.onShortcutKey(DirKeyCodes.a + (codeUnit - 97), _stickyMod);
        _imeCtrl.clear();
        setState(() => _stickyMod = 0);
        return;
      }
    }
    widget.onSendUnicodeText(txt);
    _imeCtrl.clear();
  }

  @override
  Widget build(BuildContext context) {
    final viewInsets = MediaQuery.of(context).viewInsets;

    return Stack(
      fit: StackFit.expand,
      children: [
        // 1. Mobile Precision Trackpad Layer (skipped on desktop so native mouse works directly!)
        if (!widget.isDesktop && widget.mode == MobileInputMode.trackpadRelative)
          LayoutBuilder(
            builder: (context, constraints) {
              final w = math.max(320.0, constraints.maxWidth);
              final h = math.max(240.0, constraints.maxHeight);
              return GestureDetector(
                behavior: HitTestBehavior.translucent,
                onPanUpdate: (details) {
                  // Pro precision curve: ultra-fine 0.38x at tiny slow finger nudges, smoothly ramps to 1.8x!
                  final speed = details.delta.distance;
                  final curve = speed < 2.2
                      ? 0.38 + (speed / 2.2) * 0.28
                      : math.min(1.85, 0.66 + (speed - 2.2) * 0.065);
                  final scale = curve * widget.sensitivity.multiplier;
                  widget.onRelativeTrackpadDelta(
                    (details.delta.dx * scale) / w,
                    (details.delta.dy * scale) / h,
                  );
                },
                onTap: () {
                  HapticFeedback.selectionClick();
                  widget.onTrackpadClick(0x01);
                },
                onDoubleTap: () {
                  HapticFeedback.selectionClick();
                  widget.onTrackpadClick(0x01);
                  Future.delayed(const Duration(milliseconds: 55), () {
                    widget.onTrackpadClick(0x01);
                  });
                },
                onLongPress: () {
                  HapticFeedback.mediumImpact();
                  widget.onTrackpadClick(0x02);
                },
              );
            },
          ),

        // 2. Full Dual-Stick + Shoulder/Trigger Virtual Gamepad Overlay
        if (widget.mode == MobileInputMode.virtualGamepad) ...[
          // Top-left Shoulder & Trigger (LB / LT)
          Positioned(
            left: 20,
            top: 66,
            child: Row(
              children: [
                _shoulderBtn('LT 右键', _ltVal > 0, (p) => _toggleTrigger(isLeft: true, pressed: p)),
                const SizedBox(width: 8),
                _shoulderBtn('LB (Q)', (_buttonsMask & 0x10) != 0, (p) => _toggleButton(0x10, p)),
              ],
            ),
          ),
          // Top-right Shoulder & Trigger (RB / RT)
          Positioned(
            right: 20,
            top: 66,
            child: Row(
              children: [
                _shoulderBtn('RB (R)', (_buttonsMask & 0x20) != 0, (p) => _toggleButton(0x20, p)),
                const SizedBox(width: 8),
                _shoulderBtn('RT 左键', _rtVal > 0, (p) => _toggleTrigger(isLeft: false, pressed: p)),
              ],
            ),
          ),

          // Left Analog Stick (WASD movement)
          Positioned(
            left: 24,
            bottom: 68,
            child: _buildStick(
              label: 'WASD',
              value: _leftStick,
              accent: const Color(0xFF38EF7D),
              onChanged: (v) {
                setState(() => _leftStick = v);
                _emitGamepad();
              },
            ),
          ),

          // Right Analog Stick (Camera / Relative Mouse)
          Positioned(
            right: 146,
            bottom: 64,
            child: _buildStick(
              label: '视角',
              value: _rightStick,
              size: 92,
              accent: const Color(0xFF38BDF8),
              onChanged: (v) {
                setState(() => _rightStick = v);
                _syncRightStickLoop();
                _emitGamepad();
              },
            ),
          ),

          // Right ABXY Face Buttons (Space / Esc / E / F)
          Positioned(
            right: 18,
            bottom: 68,
            child: SizedBox(
              width: 118,
              child: Wrap(
                spacing: 10,
                runSpacing: 10,
                children: [
                  _padBtn('A', '跳', 0x01),
                  _padBtn('B', 'Esc', 0x02),
                  _padBtn('X', 'E', 0x04),
                  _padBtn('Y', 'F', 0x08),
                ],
              ),
            ),
          ),
        ],

        // 3. Android / Mobile Native Soft Keyboard IME Input Bar
        if (widget.keyboardVisible)
          Positioned(
            left: 0,
            right: 0,
            bottom: viewInsets.bottom,
            child: SafeArea(
              top: false,
              child: Container(
                padding: const EdgeInsets.fromLTRB(10, 8, 10, 8),
                decoration: BoxDecoration(
                  color: const Color(0xF2121824),
                  border: const Border(top: BorderSide(color: Colors.white12)),
                  boxShadow: [
                    BoxShadow(
                      color: Colors.black.withValues(alpha: 0.45),
                      blurRadius: 12,
                      offset: const Offset(0, -3),
                    ),
                  ],
                ),
                child: Column(
                  mainAxisSize: MainAxisSize.min,
                  children: [
                    // Special Keys & Sticky Modifiers Strip
                    SingleChildScrollView(
                      scrollDirection: Axis.horizontal,
                      child: Row(
                        children: [
                          _modChip('Ctrl', 0x02),
                          _modChip('Alt', 0x04),
                          _modChip('Cmd/Win', 0x08),
                          _divider(),
                          _keyChip('Esc', DirKeyCodes.escape, 0),
                          _keyChip('Tab', DirKeyCodes.tab, 0),
                          _keyChip('←', DirKeyCodes.left, 0),
                          _keyChip('↑', DirKeyCodes.up, 0),
                          _keyChip('↓', DirKeyCodes.down, 0),
                          _keyChip('→', DirKeyCodes.right, 0),
                          _keyChip('退格', DirKeyCodes.backspace, 0),
                          _keyChip('Del', DirKeyCodes.delete, 0),
                          _keyChip('回车', DirKeyCodes.enter, 0),
                          _divider(),
                          _keyChip('全选', DirKeyCodes.a, 0x08),
                          _keyChip('复制', DirKeyCodes.c, 0x08),
                          _keyChip('粘贴', DirKeyCodes.v, 0x08),
                          _keyChip('撤销', DirKeyCodes.z, 0x08),
                        ],
                      ),
                    ),
                    const SizedBox(height: 6),
                    // Native IME TextField (Supports Chinese Pinyin, English & Emoji!)
                    Row(
                      children: [
                        Expanded(
                          child: TextField(
                            controller: _imeCtrl,
                            focusNode: _imeFocus,
                            autofocus: true,
                            autocorrect: false,
                            textInputAction: TextInputAction.send,
                            onSubmitted: (_) => _sendImeBuffer(),
                            style: const TextStyle(fontSize: 14, color: Colors.white),
                            decoration: InputDecoration(
                              hintText: '输入中文 / 英文 / 符号后点发送...',
                              hintStyle: const TextStyle(color: Colors.white38, fontSize: 13),
                              isDense: true,
                              contentPadding: const EdgeInsets.symmetric(horizontal: 12, vertical: 9),
                              filled: true,
                              fillColor: const Color(0xFF1B2333),
                              border: OutlineInputBorder(
                                borderRadius: BorderRadius.circular(12),
                                borderSide: const BorderSide(color: Colors.white12),
                              ),
                            ),
                          ),
                        ),
                        const SizedBox(width: 8),
                        FilledButton(
                          style: FilledButton.styleFrom(
                            backgroundColor: const Color(0xFF38EF7D),
                            foregroundColor: Colors.black,
                            padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 10),
                            minimumSize: const Size(56, 38),
                          ),
                          onPressed: _sendImeBuffer,
                          child: const Text('发送', style: TextStyle(fontWeight: FontWeight.w700, fontSize: 13)),
                        ),
                        const SizedBox(width: 4),
                        IconButton(
                          visualDensity: VisualDensity.compact,
                          tooltip: '收起键盘',
                          icon: const Icon(Icons.keyboard_hide_rounded, color: Colors.white70),
                          onPressed: widget.onToggleKeyboard,
                        ),
                      ],
                    ),
                  ],
                ),
              ),
            ),
          ),

        // 4. Collapsible Bottom Floating Quick-Action Dock (hidden while keyboard is open)
        if (!widget.keyboardVisible)
          Positioned(
            left: 12,
            right: 12,
            bottom: 10,
            child: SafeArea(
              top: false,
              child: Center(
                child: _dockCollapsed
                    ? ActionChip(
                        backgroundColor: const Color(0xCC131924),
                        avatar: const Icon(Icons.keyboard_arrow_up_rounded, size: 16),
                        label: const Text('展开快捷栏', style: TextStyle(fontSize: 11)),
                        onPressed: () => setState(() => _dockCollapsed = false),
                      )
                    : SingleChildScrollView(
                        scrollDirection: Axis.horizontal,
                        child: Container(
                          padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 4),
                          decoration: BoxDecoration(
                            color: const Color(0xDD111722),
                            borderRadius: BorderRadius.circular(20),
                            border: Border.all(color: Colors.white12),
                          ),
                          child: Row(
                            mainAxisSize: MainAxisSize.min,
                            children: [
                              _actionBtn('键盘', Icons.keyboard_alt_rounded, widget.onToggleKeyboard),
                              _divider(),
                              _actionBtn('左键', Icons.ads_click_rounded, () => widget.onTrackpadClick(0x01)),
                              _actionBtn('右键', Icons.touch_app_outlined, () => widget.onTrackpadClick(0x02)),
                              _actionBtn('上滚', Icons.keyboard_arrow_up_rounded, () => widget.onTrackpadScroll(120)),
                              _actionBtn('下滚', Icons.keyboard_arrow_down_rounded, () => widget.onTrackpadScroll(-120)),
                              if (!widget.isDesktop && widget.mode == MobileInputMode.trackpadRelative) ...[
                                _divider(),
                                _actionBtn(
                                  '灵敏:${widget.sensitivity.label}',
                                  Icons.tune_rounded,
                                  widget.onCycleSensitivity,
                                ),
                              ],
                              _divider(),
                              _keyChip('Esc', DirKeyCodes.escape, 0),
                              _keyChip('Tab', DirKeyCodes.tab, 0),
                              _keyChip('Win/Cmd', DirKeyCodes.leftMeta, 0x08),
                              _keyChip('复制', DirKeyCodes.c, 0x08),
                              _keyChip('粘贴', DirKeyCodes.v, 0x08),
                              IconButton(
                                visualDensity: VisualDensity.compact,
                                padding: EdgeInsets.zero,
                                constraints: const BoxConstraints(minWidth: 28, minHeight: 28),
                                icon: const Icon(Icons.expand_more_rounded, size: 18, color: Colors.white54),
                                onPressed: () => setState(() => _dockCollapsed = true),
                              ),
                            ],
                          ),
                        ),
                      ),
              ),
            ),
          ),
      ],
    );
  }

  Widget _buildStick({
    required String label,
    required Offset value,
    required Color accent,
    required void Function(Offset next) onChanged,
    double size = 112,
  }) {
    final radius = size * 0.44;
    return GestureDetector(
      onPanUpdate: (d) {
        final raw = Offset(
          value.dx + d.delta.dx / radius,
          value.dy + d.delta.dy / radius,
        );
        final dist = raw.distance;
        final clamped = dist > 1.0 ? Offset(raw.dx / dist, raw.dy / dist) : raw;
        onChanged(clamped);
      },
      onPanEnd: (_) => onChanged(Offset.zero),
      onPanCancel: () => onChanged(Offset.zero),
      child: Container(
        width: size,
        height: size,
        decoration: BoxDecoration(
          shape: BoxShape.circle,
          color: Colors.white.withValues(alpha: 0.10),
          border: Border.all(color: accent.withValues(alpha: 0.55), width: 1.5),
        ),
        child: Stack(
          alignment: Alignment.center,
          children: [
            Text(
              label,
              style: TextStyle(
                color: Colors.white.withValues(alpha: 0.32),
                fontSize: 10.5,
                fontWeight: FontWeight.w700,
              ),
            ),
            Align(
              alignment: Alignment(value.dx, value.dy),
              child: Container(
                width: size * 0.38,
                height: size * 0.38,
                decoration: BoxDecoration(
                  shape: BoxShape.circle,
                  color: accent.withValues(alpha: 0.90),
                  boxShadow: [
                    BoxShadow(color: accent.withValues(alpha: 0.4), blurRadius: 8),
                  ],
                ),
              ),
            ),
          ],
        ),
      ),
    );
  }

  Widget _shoulderBtn(String label, bool active, void Function(bool pressed) onToggle) {
    return GestureDetector(
      onTapDown: (_) => onToggle(true),
      onTapUp: (_) => onToggle(false),
      onTapCancel: () => onToggle(false),
      child: Container(
        padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 7),
        decoration: BoxDecoration(
          color: active ? const Color(0xFF38EF7D) : const Color(0xCC131924),
          borderRadius: BorderRadius.circular(14),
          border: Border.all(color: active ? const Color(0xFF38EF7D) : Colors.white24),
        ),
        child: Text(
          label,
          style: TextStyle(
            color: active ? Colors.black : Colors.white,
            fontSize: 11.5,
            fontWeight: FontWeight.w700,
          ),
        ),
      ),
    );
  }

  Widget _modChip(String label, int bit) {
    final active = (_stickyMod & bit) != 0;
    return Padding(
      padding: const EdgeInsets.only(right: 5),
      child: InkWell(
        onTap: () {
          setState(() {
            if (active) {
              _stickyMod &= ~bit;
            } else {
              _stickyMod |= bit;
            }
          });
        },
        borderRadius: BorderRadius.circular(8),
        child: Container(
          padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 4),
          decoration: BoxDecoration(
            color: active ? const Color(0xFF38EF7D) : Colors.white10,
            borderRadius: BorderRadius.circular(8),
          ),
          child: Text(
            label,
            style: TextStyle(
              color: active ? Colors.black : Colors.white70,
              fontSize: 11.5,
              fontWeight: FontWeight.w600,
            ),
          ),
        ),
      ),
    );
  }

  Widget _divider() => Container(
        width: 1,
        height: 18,
        margin: const EdgeInsets.symmetric(horizontal: 4),
        color: Colors.white12,
      );

  Widget _actionBtn(String label, IconData icon, VoidCallback onTap) {
    return InkWell(
      onTap: onTap,
      borderRadius: BorderRadius.circular(10),
      child: Padding(
        padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 5),
        child: Row(
          mainAxisSize: MainAxisSize.min,
          children: [
            Icon(icon, size: 14, color: const Color(0xFF4FFFB0)),
            const SizedBox(width: 3),
            Text(label, style: const TextStyle(color: Colors.white, fontSize: 11)),
          ],
        ),
      ),
    );
  }

  Widget _padBtn(String label, String hint, int bit) {
    final active = (_buttonsMask & bit) != 0;
    return GestureDetector(
      onTapDown: (_) => _toggleButton(bit, true),
      onTapUp: (_) => _toggleButton(bit, false),
      onTapCancel: () => _toggleButton(bit, false),
      child: Container(
        width: 50,
        height: 50,
        alignment: Alignment.center,
        decoration: BoxDecoration(
          shape: BoxShape.circle,
          color: active ? const Color(0xFF38EF7D) : Colors.white.withValues(alpha: 0.16),
          border: Border.all(color: active ? const Color(0xFF38EF7D) : Colors.white38),
        ),
        child: Column(
          mainAxisAlignment: MainAxisAlignment.center,
          children: [
            Text(
              label,
              style: TextStyle(
                color: active ? Colors.black : Colors.white,
                fontWeight: FontWeight.bold,
                fontSize: 14,
              ),
            ),
            Text(
              hint,
              style: TextStyle(
                color: active ? Colors.black87 : Colors.white60,
                fontSize: 9,
              ),
            ),
          ],
        ),
      ),
    );
  }

  Widget _keyChip(String label, int keycode, int mod) {
    return InkWell(
      onTap: () {
        HapticFeedback.selectionClick();
        widget.onShortcutKey(keycode, mod | _stickyMod);
        if (_stickyMod != 0) {
          setState(() => _stickyMod = 0);
        }
      },
      borderRadius: BorderRadius.circular(8),
      child: Padding(
        padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 5),
        child: Text(
          label,
          style: const TextStyle(color: Colors.white70, fontSize: 11.5),
        ),
      ),
    );
  }
}
