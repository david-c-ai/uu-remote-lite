import 'dart:ui' as ui;
import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import '../models/session_models.dart';

class RemoteCanvas extends StatefulWidget {
  const RemoteCanvas({
    super.key,
    required this.frameImage,
    required this.telemetry,
    required this.localCursorNorm,
    required this.scaleMode,
    required this.enablePinchZoom,
    required this.capturePhysicalKeyboard,
    required this.clickRippleSeq,
    required this.onPointerEventNorm,
    required this.onDirectTouchTapNorm,
    required this.onPhysicalKey,
  });

  final ui.Image? frameImage;
  final TelemetrySnapshot telemetry;
  final Offset localCursorNorm;
  final ViewportScaleMode scaleMode;
  final bool enablePinchZoom;
  final bool capturePhysicalKeyboard;
  final int clickRippleSeq;
  final void Function(int xNorm, int yNorm, int buttonMask, int wheelY) onPointerEventNorm;
  final void Function(int xNorm, int yNorm) onDirectTouchTapNorm;
  final void Function(int dirKeyCode, bool isDown, int modifiers) onPhysicalKey;

  @override
  State<RemoteCanvas> createState() => _RemoteCanvasState();
}

class _RemoteCanvasState extends State<RemoteCanvas> with SingleTickerProviderStateMixin {
  bool _expandedHud = false;
  final TransformationController _zoomCtrl = TransformationController();
  final FocusNode _kbFocus = FocusNode(debugLabel: 'DirRemoteCanvasFocus');
  late final AnimationController _rippleCtrl;

  @override
  void initState() {
    super.initState();
    _rippleCtrl = AnimationController(
      vsync: this,
      duration: const Duration(milliseconds: 220),
    );
  }

  @override
  void didUpdateWidget(covariant RemoteCanvas oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (widget.clickRippleSeq != oldWidget.clickRippleSeq) {
      _rippleCtrl.forward(from: 0.0);
    }
    if (!widget.enablePinchZoom && oldWidget.enablePinchZoom) {
      _zoomCtrl.value = Matrix4.identity();
    }
  }

  @override
  void dispose() {
    _rippleCtrl.dispose();
    _zoomCtrl.dispose();
    _kbFocus.dispose();
    super.dispose();
  }

  int _mapButtons(int flutterButtons) {
    int mask = 0;
    if ((flutterButtons & kPrimaryMouseButton) != 0) mask |= 0x01;
    if ((flutterButtons & kSecondaryMouseButton) != 0) mask |= 0x02;
    if ((flutterButtons & kMiddleMouseButton) != 0) mask |= 0x04;
    return mask;
  }

  int _currentModifiers() {
    final hw = HardwareKeyboard.instance;
    int m = 0;
    if (hw.isShiftPressed) m |= 0x01;
    if (hw.isControlPressed) m |= 0x02;
    if (hw.isAltPressed) m |= 0x04;
    if (hw.isMetaPressed) m |= 0x08;
    return m;
  }

  KeyEventResult _handleKeyEvent(FocusNode node, KeyEvent event) {
    if (!widget.capturePhysicalKeyboard) return KeyEventResult.ignored;
    final hid = event.physicalKey.usbHidUsage;
    // USB HID Keyboard Usage Page (0x0007xxxx) maps 1:1 to canonical DirKey!
    if ((hid >> 16) != 0x0007) return KeyEventResult.ignored;
    final dirKey = hid & 0xFFFF;
    if (dirKey == 0) return KeyEventResult.ignored;

    final isDown = event is KeyDownEvent || event is KeyRepeatEvent;
    final isUp = event is KeyUpEvent;
    if (!isDown && !isUp) return KeyEventResult.ignored;

    widget.onPhysicalKey(dirKey, isDown, _currentModifiers());
    return KeyEventResult.handled;
  }

  Rect _computeDisplayRect(Size viewport) {
    if (widget.scaleMode == ViewportScaleMode.fill || widget.frameImage == null) {
      return Offset.zero & viewport;
    }
    final imgW = widget.frameImage!.width.toDouble();
    final imgH = widget.frameImage!.height.toDouble();
    if (imgW <= 0 || imgH <= 0) return Offset.zero & viewport;

    final scale = (viewport.width / imgW < viewport.height / imgH)
        ? (viewport.width / imgW)
        : (viewport.height / imgH);
    final rw = imgW * scale;
    final rh = imgH * scale;
    final left = (viewport.width - rw) * 0.5;
    final top = (viewport.height - rh) * 0.5;
    return Rect.fromLTWH(left, top, rw, rh);
  }

  @override
  Widget build(BuildContext context) {
    return Focus(
      focusNode: _kbFocus,
      autofocus: widget.capturePhysicalKeyboard,
      onKeyEvent: _handleKeyEvent,
      child: LayoutBuilder(
        builder: (context, constraints) {
          final viewport = Size(constraints.maxWidth, constraints.maxHeight);
          final rect = _computeDisplayRect(viewport);

          Offset toSceneIfZoomed(Offset localPos) {
            return widget.enablePinchZoom ? _zoomCtrl.toScene(localPos) : localPos;
          }

          int normX(Offset localPos) {
            final p = toSceneIfZoomed(localPos);
            return (((p.dx - rect.left) / (rect.width > 1 ? rect.width : 1)).clamp(0.0, 1.0) * 65535).round();
          }

          int normY(Offset localPos) {
            final p = toSceneIfZoomed(localPos);
            return (((p.dy - rect.top) / (rect.height > 1 ? rect.height : 1)).clamp(0.0, 1.0) * 65535).round();
          }

          final cx = rect.left + widget.localCursorNorm.dx.clamp(0.0, 1.0) * rect.width;
          final cy = rect.top + widget.localCursorNorm.dy.clamp(0.0, 1.0) * rect.height;

          Widget videoSurface = Stack(
            fit: StackFit.expand,
            children: [
              Container(
                color: const Color(0xFF0A0D12),
                child: widget.frameImage != null
                    ? RawImage(
                        image: widget.frameImage,
                        fit: widget.scaleMode == ViewportScaleMode.fill
                            ? BoxFit.fill
                            : BoxFit.contain,
                        filterQuality: FilterQuality.medium,
                      )
                    : Center(
                        child: Column(
                          mainAxisSize: MainAxisSize.min,
                          children: [
                            const SizedBox(
                              width: 28,
                              height: 28,
                              child: CircularProgressIndicator(strokeWidth: 2.4),
                            ),
                            const SizedBox(height: 14),
                            Text(
                              '已连接 · 正在同步屏幕 ${widget.telemetry.activeMonitor + 1} 画面...',
                              style: const TextStyle(color: Colors.white60, fontSize: 13),
                            ),
                          ],
                        ),
                      ),
              ),

              // Precision Click Ripple centered at exact hotspot (cx, cy)
              AnimatedBuilder(
                animation: _rippleCtrl,
                builder: (_, __) {
                  if (_rippleCtrl.value == 0.0 || _rippleCtrl.value == 1.0) {
                    return const SizedBox.shrink();
                  }
                  final t = _rippleCtrl.value;
                  final radius = 4.0 + t * 14.0;
                  return Positioned(
                    left: cx - radius,
                    top: cy - radius,
                    child: IgnorePointer(
                      child: Container(
                        width: radius * 2,
                        height: radius * 2,
                        decoration: BoxDecoration(
                          shape: BoxShape.circle,
                          border: Border.all(
                            color: const Color(0xFF38EF7D).withValues(alpha: (1.0 - t) * 0.9),
                            width: 2.0,
                          ),
                        ),
                      ),
                    ),
                  );
                },
              ),

              // Precision 1px-Hotspot Arrow Cursor: Tip is strictly at (cx, cy) = (0, 0)!
              Positioned(
                left: cx,
                top: cy,
                child: const IgnorePointer(
                  child: CustomPaint(
                    size: Size(20, 24),
                    painter: _PrecisionCursorPainter(),
                  ),
                ),
              ),
            ],
          );

          if (widget.enablePinchZoom) {
            videoSurface = GestureDetector(
              behavior: HitTestBehavior.translucent,
              onTapUp: (details) {
                widget.onDirectTouchTapNorm(
                  normX(details.localPosition),
                  normY(details.localPosition),
                );
              },
              child: InteractiveViewer(
                transformationController: _zoomCtrl,
                minScale: 1.0,
                maxScale: 4.5,
                panEnabled: true,
                scaleEnabled: true,
                child: videoSurface,
              ),
            );
          }

          final tele = widget.telemetry;
          final monText = tele.monitorCount > 1
              ? ' · 屏${tele.activeMonitor + 1}/${tele.monitorCount}'
              : '';

          return Listener(
            onPointerHover: (ev) => widget.onPointerEventNorm(
              normX(ev.localPosition),
              normY(ev.localPosition),
              0,
              0,
            ),
            onPointerMove: (ev) {
              if (ev.kind == PointerDeviceKind.mouse) {
                widget.onPointerEventNorm(
                  normX(ev.localPosition),
                  normY(ev.localPosition),
                  _mapButtons(ev.buttons),
                  0,
                );
              }
            },
            onPointerDown: (ev) {
              if (widget.capturePhysicalKeyboard && !_kbFocus.hasFocus) {
                _kbFocus.requestFocus();
              }
              if (ev.kind == PointerDeviceKind.mouse) {
                widget.onPointerEventNorm(
                  normX(ev.localPosition),
                  normY(ev.localPosition),
                  _mapButtons(ev.buttons),
                  0,
                );
              }
            },
            onPointerUp: (ev) {
              if (ev.kind == PointerDeviceKind.mouse) {
                widget.onPointerEventNorm(
                  normX(ev.localPosition),
                  normY(ev.localPosition),
                  0,
                  0,
                );
              }
            },
            onPointerSignal: (sig) {
              if (sig is PointerScrollEvent) {
                widget.onPointerEventNorm(
                  normX(sig.localPosition),
                  normY(sig.localPosition),
                  0,
                  (-sig.scrollDelta.dy).round(),
                );
              }
            },
            child: MouseRegion(
              cursor: SystemMouseCursors.none,
              child: Stack(
                fit: StackFit.expand,
                children: [
                  videoSurface,

                  // Compact, non-overflowing Tap-to-Expand Telemetry Pill
                  Positioned(
                    top: 10,
                    left: 12,
                    right: 12,
                    child: Align(
                      alignment: Alignment.topLeft,
                      child: GestureDetector(
                        onTap: () => setState(() => _expandedHud = !_expandedHud),
                        child: Container(
                          padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 5),
                          decoration: BoxDecoration(
                            color: const Color(0xCC10151F),
                            borderRadius: BorderRadius.circular(18),
                            border: Border.all(color: Colors.white12),
                          ),
                          child: Row(
                            mainAxisSize: MainAxisSize.min,
                            children: [
                              Container(
                                width: 7,
                                height: 7,
                                decoration: BoxDecoration(
                                  shape: BoxShape.circle,
                                  color: tele.transportPath == 1
                                      ? const Color(0xFF38EF7D)
                                      : Colors.amberAccent,
                                ),
                              ),
                              const SizedBox(width: 6),
                              Flexible(
                                child: Text(
                                  _expandedHud
                                      ? '${tele.pathBadge} · ${tele.codecBadge} · ${tele.rttMs.toStringAsFixed(1)}ms · '
                                          '${tele.formattedBandwidth} · ${tele.currentFps.toStringAsFixed(0)}fps · '
                                          '${tele.remoteWidth}x${tele.remoteHeight}$monText'
                                      : '${tele.pathBadge} · ${tele.codecBadge} · ${tele.rttMs.toStringAsFixed(0)}ms$monText',
                                  maxLines: 1,
                                  overflow: TextOverflow.ellipsis,
                                  style: const TextStyle(
                                    color: Colors.white,
                                    fontSize: 11,
                                    fontFamily: 'monospace',
                                  ),
                                ),
                              ),
                            ],
                          ),
                        ),
                      ),
                    ),
                  ),
                ],
              ),
            ),
          );
        },
      ),
    );
  }
}

/// Crisp OS-grade pointer arrow whose exact click hotspot is strictly at (0, 0).
class _PrecisionCursorPainter extends CustomPainter {
  const _PrecisionCursorPainter();

  @override
  void paint(Canvas canvas, Size size) {
    final path = Path()
      ..moveTo(0, 0)
      ..lineTo(0, 17.5)
      ..lineTo(4.4, 13.6)
      ..lineTo(7.8, 20.6)
      ..lineTo(10.6, 19.2)
      ..lineTo(7.2, 12.2)
      ..lineTo(12.8, 12.2)
      ..close();

    // Subtle shadow for visibility on both pure white and pure black backgrounds
    canvas.drawShadow(path.shift(const Offset(0.8, 1.0)), Colors.black, 3.0, false);

    final fillPaint = Paint()
      ..color = Colors.white
      ..style = PaintingStyle.fill;
    canvas.drawPath(path, fillPaint);

    final borderPaint = Paint()
      ..color = const Color(0xFF0E1722)
      ..style = PaintingStyle.stroke
      ..strokeWidth = 1.4
      ..strokeJoin = StrokeJoin.round;
    canvas.drawPath(path, borderPaint);

    // Pinpoint emerald micro-dot right behind tip so the exact click pixel is unmistakable
    final tipDot = Paint()..color = const Color(0xFF00E676);
    canvas.drawCircle(const Offset(2.2, 3.4), 1.35, tipDot);
  }

  @override
  bool shouldRepaint(covariant CustomPainter oldDelegate) => false;
}
