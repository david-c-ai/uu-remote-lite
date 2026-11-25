import 'dart:math' as math;
import 'package:flutter/material.dart';
import '../models/session_models.dart';

class MobileTouchOverlay extends StatefulWidget {
  const MobileTouchOverlay({
    super.key,
    required this.mode,
    required this.onRelativeTrackpadDelta,
    required this.onTrackpadClick,
    required this.onTrackpadScroll,
    required this.onDirectTouchTapNorm,
    required this.onShortcutKey,
    required this.onGamepadUpdate,
  });

  final MobileInputMode mode;
  final void Function(double dxNorm, double dyNorm) onRelativeTrackpadDelta;
  final void Function(int buttonMask) onTrackpadClick;
  final void Function(int wheelDelta) onTrackpadScroll;
  final void Function(int xNorm, int yNorm) onDirectTouchTapNorm;
  final void Function(int keycode, int modifiers) onShortcutKey;
  final void Function(int buttons, int lx, int ly, int rx, int ry) onGamepadUpdate;

  @override
  State<MobileTouchOverlay> createState() => _MobileTouchOverlayState();
}

class _MobileTouchOverlayState extends State<MobileTouchOverlay> {
  int _buttonsMask = 0;
  Offset _leftStick = Offset.zero;
  bool _dockCollapsed = false;

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
        // Trackpad gesture layer (only captures pan in trackpad mode; directTouchZoom lets InteractiveViewer zoom!)
        if (widget.mode == MobileInputMode.trackpadRelative)
          LayoutBuilder(
            builder: (context, constraints) {
              return GestureDetector(
                behavior: HitTestBehavior.translucent,
                onPanUpdate: (details) {
                  final speed = details.delta.distance;
                  final refDim = math.max(320.0, math.min(constraints.maxWidth, constraints.maxHeight));
                  final gain = (0.95 + math.min(2.2, speed * 0.08)) / refDim;
                  widget.onRelativeTrackpadDelta(
                    details.delta.dx * gain,
                    details.delta.dy * gain,
                  );
                },
                onTap: () => widget.onTrackpadClick(0x01),
                onDoubleTap: () {
                  widget.onTrackpadClick(0x01);
                  Future.delayed(const Duration(milliseconds: 60), () {
                    widget.onTrackpadClick(0x01);
                  });
                },
                onLongPress: () => widget.onTrackpadClick(0x02),
              );
            },
          )
        else if (widget.mode == MobileInputMode.directTouchZoom)
          LayoutBuilder(
            builder: (context, constraints) {
              return GestureDetector(
                behavior: HitTestBehavior.translucent,
                onTapUp: (details) {
                  final xn = ((details.localPosition.dx / constraints.maxWidth).clamp(0.0, 1.0) * 65535).round();
                  final yn = ((details.localPosition.dy / constraints.maxHeight).clamp(0.0, 1.0) * 65535).round();
                  widget.onDirectTouchTapNorm(xn, yn);
                },
              );
            },
          ),

        // Virtual Gamepad Overlay
        if (widget.mode == MobileInputMode.virtualGamepad) ...[
          Positioned(
            left: 24,
            bottom: 64,
            child: GestureDetector(
              onPanUpdate: (d) {
                final clamped = Offset(
                  (_leftStick.dx + d.delta.dx / 52.0).clamp(-1.0, 1.0),
                  (_leftStick.dy + d.delta.dy / 52.0).clamp(-1.0, 1.0),
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
                width: 112,
                height: 112,
                decoration: BoxDecoration(
                  shape: BoxShape.circle,
                  color: Colors.white.withValues(alpha: 0.10),
                  border: Border.all(color: Colors.white30),
                ),
                alignment: Alignment(_leftStick.dx, _leftStick.dy),
                child: Container(
                  width: 42,
                  height: 42,
                  decoration: const BoxDecoration(
                    shape: BoxShape.circle,
                    color: Color(0xFF38EF7D),
                  ),
                ),
              ),
            ),
          ),
          Positioned(
            right: 24,
            bottom: 68,
            child: SizedBox(
              width: 116,
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
          ),
        ],

        // Collapsible Bottom Floating Quick-Action Dock
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
                            _actionBtn('左键', Icons.ads_click_rounded, () => widget.onTrackpadClick(0x01)),
                            _actionBtn('右键', Icons.touch_app_outlined, () => widget.onTrackpadClick(0x02)),
                            _actionBtn('上滚', Icons.keyboard_arrow_up_rounded, () => widget.onTrackpadScroll(120)),
                            _actionBtn('下滚', Icons.keyboard_arrow_down_rounded, () => widget.onTrackpadScroll(-120)),
                            _divider(),
                            _keyChip('Esc', 53, 0),
                            _keyChip('Tab', 48, 0),
                            _keyChip('Win/Cmd', 55, 0x08),
                            _keyChip('复制', 8, 0x08),
                            _keyChip('粘贴', 9, 0x08),
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

  Widget _padBtn(String label, int bit) {
    final active = (_buttonsMask & bit) != 0;
    return GestureDetector(
      onTapDown: (_) => _toggleButton(bit, true),
      onTapUp: (_) => _toggleButton(bit, false),
      onTapCancel: () => _toggleButton(bit, false),
      child: Container(
        width: 46,
        height: 46,
        alignment: Alignment.center,
        decoration: BoxDecoration(
          shape: BoxShape.circle,
          color: active ? const Color(0xFF38EF7D) : Colors.white.withValues(alpha: 0.16),
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
    return InkWell(
      onTap: () => widget.onShortcutKey(keycode, mod),
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
