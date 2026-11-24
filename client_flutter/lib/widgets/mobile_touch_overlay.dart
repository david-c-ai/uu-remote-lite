import 'dart:math' as math;
import 'package:flutter/material.dart';
import '../models/session_models.dart';

class MobileTouchOverlay extends StatefulWidget {
  const MobileTouchOverlay({
    super.key,
    required this.mode,
    required this.onRelativeTrackpadDelta,
    required this.onTrackpadClick,
    required this.onDirectTouchTapNorm,
    required this.onShortcutKey,
    required this.onGamepadUpdate,
  });

  final MobileInputMode mode;
  final void Function(double dxNorm, double dyNorm) onRelativeTrackpadDelta;
  final void Function(int buttonMask) onTrackpadClick;
  final void Function(int xNorm, int yNorm) onDirectTouchTapNorm;
  final void Function(int keycode, int modifiers) onShortcutKey;
  final void Function(int buttons, int lx, int ly, int rx, int ry) onGamepadUpdate;

  @override
  State<MobileTouchOverlay> createState() => _MobileTouchOverlayState();
}

class _MobileTouchOverlayState extends State<MobileTouchOverlay> {
  int _buttonsMask = 0;
  Offset _leftStick = Offset.zero;

  void _toggleButton(int bit, bool pressed) {
    setState(() {
      if (pressed) {
        _buttonsMask |= bit;
      } else {
        _buttonsMask &= ~bit;
      }
    });
    widget.onGamepadUpdate(
      _buttonsMask,
      (_leftStick.dx * 32767).round(),
      (_leftStick.dy * 32767).round(),
      0,
      0,
    );
  }

  @override
  Widget build(BuildContext context) {
    return Stack(
      fit: StackFit.expand,
      children: [
        // Gesture layer: Velocity-accelerated relative trackpad OR Direct touch
        LayoutBuilder(
          builder: (context, constraints) {
            return GestureDetector(
              behavior: HitTestBehavior.translucent,
              onPanUpdate: (details) {
                if (widget.mode == MobileInputMode.trackpadRelative) {
                  final speed = details.delta.distance;
                  // Pointer acceleration curve: fine pixel control when slow, rapid sweep when fast
                  final gain = (1.0 + math.min(2.4, speed * 0.08)) / constraints.maxWidth;
                  widget.onRelativeTrackpadDelta(
                    details.delta.dx * gain,
                    details.delta.dy * gain,
                  );
                }
              },
              onTap: () {
                if (widget.mode == MobileInputMode.trackpadRelative) {
                  widget.onTrackpadClick(0x01); // Single-finger tap = Left Click
                }
              },
              onDoubleTap: () {
                if (widget.mode == MobileInputMode.trackpadRelative) {
                  widget.onTrackpadClick(0x02); // Double tap / Context = Right Click
                }
              },
              onTapDown: (details) {
                if (widget.mode == MobileInputMode.directTouch) {
                  final xn = ((details.localPosition.dx / constraints.maxWidth).clamp(0.0, 1.0) * 65535).round();
                  final yn = ((details.localPosition.dy / constraints.maxHeight).clamp(0.0, 1.0) * 65535).round();
                  widget.onDirectTouchTapNorm(xn, yn);
                }
              },
            );
          },
        ),

        // Virtual Gamepad Overlay (UU Remote Gaming Mode)
        if (widget.mode == MobileInputMode.virtualGamepad) ...[
          Positioned(
            left: 28,
            bottom: 68,
            child: GestureDetector(
              onPanUpdate: (d) {
                final clamped = Offset(
                  (_leftStick.dx + d.delta.dx / 55.0).clamp(-1.0, 1.0),
                  (_leftStick.dy + d.delta.dy / 55.0).clamp(-1.0, 1.0),
                );
                setState(() => _leftStick = clamped);
                widget.onGamepadUpdate(
                  _buttonsMask,
                  (clamped.dx * 32767).round(),
                  (clamped.dy * 32767).round(),
                  0,
                  0,
                );
              },
              onPanEnd: (_) {
                setState(() => _leftStick = Offset.zero);
                widget.onGamepadUpdate(_buttonsMask, 0, 0, 0, 0);
              },
              child: Container(
                width: 116,
                height: 116,
                decoration: BoxDecoration(
                  shape: BoxShape.circle,
                  color: Colors.white.withValues(alpha: 0.10),
                  border: Border.all(color: Colors.white30),
                ),
                alignment: Alignment(_leftStick.dx, _leftStick.dy),
                child: Container(
                  width: 44,
                  height: 44,
                  decoration: const BoxDecoration(
                    shape: BoxShape.circle,
                    color: Colors.tealAccent,
                  ),
                ),
              ),
            ),
          ),
          Positioned(
            right: 28,
            bottom: 72,
            child: Wrap(
              spacing: 12,
              runSpacing: 12,
              children: [
                _padBtn('A', 0x01),
                _padBtn('B', 0x02),
                _padBtn('X', 0x04),
                _padBtn('Y', 0x08),
              ],
            ),
          ),
        ],

        // Bottom Floating Shortcut Key Bar (Essential for iOS/Android Desktop Control)
        Positioned(
          left: 16,
          right: 16,
          bottom: 12,
          child: Center(
            child: SingleChildScrollView(
              scrollDirection: Axis.horizontal,
              child: Container(
                padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 4),
                decoration: BoxDecoration(
                  color: Colors.black.withValues(alpha: 0.78),
                  borderRadius: BorderRadius.circular(16),
                  border: Border.all(color: Colors.white12),
                ),
                child: Row(
                  mainAxisSize: MainAxisSize.min,
                  children: [
                    _keyChip('Esc', 53, 0),
                    _keyChip('Tab', 48, 0),
                    _keyChip('Win/Cmd', 55, 0x08),
                    _keyChip('Copy', 8, 0x08),
                    _keyChip('Paste', 9, 0x08),
                    _keyChip('Ctrl+Alt+Del', 117, 0x06),
                  ],
                ),
              ),
            ),
          ),
        ),
      ],
    );
  }

  Widget _padBtn(String label, int bit) {
    final active = (_buttonsMask & bit) != 0;
    return GestureDetector(
      onTapDown: (_) => _toggleButton(bit, true),
      onTapUp: (_) => _toggleButton(bit, false),
      onTapCancel: () => _toggleButton(bit, false),
      child: Container(
        width: 48,
        height: 48,
        alignment: Alignment.center,
        decoration: BoxDecoration(
          shape: BoxShape.circle,
          color: active ? Colors.tealAccent : Colors.white.withValues(alpha: 0.16),
          border: Border.all(color: Colors.white38),
        ),
        child: Text(
          label,
          style: TextStyle(
            color: active ? Colors.black : Colors.white,
            fontWeight: FontWeight.bold,
          ),
        ),
      ),
    );
  }

  Widget _keyChip(String label, int keycode, int mod) {
    return Padding(
      padding: const EdgeInsets.symmetric(horizontal: 4),
      child: InkWell(
        onTap: () => widget.onShortcutKey(keycode, mod),
        borderRadius: BorderRadius.circular(8),
        child: Padding(
          padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 6),
          child: Text(
            label,
            style: const TextStyle(color: Colors.white70, fontSize: 12),
          ),
        ),
      ),
    );
  }
}
