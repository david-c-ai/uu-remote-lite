import 'dart:ui' as ui;
import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import '../models/session_models.dart';

class RemoteCanvas extends StatelessWidget {
  const RemoteCanvas({
    super.key,
    required this.frameImage,
    required this.telemetry,
    required this.localCursorNorm,
    required this.onPointerEventNorm,
  });

  final ui.Image? frameImage;
  final TelemetrySnapshot telemetry;
  final Offset localCursorNorm;
  final void Function(int xNorm, int yNorm, int buttonMask, int wheelY) onPointerEventNorm;

  int _mapButtons(int flutterButtons) {
    int mask = 0;
    if ((flutterButtons & kPrimaryMouseButton) != 0) mask |= 0x01;
    if ((flutterButtons & kSecondaryMouseButton) != 0) mask |= 0x02;
    if ((flutterButtons & kMiddleMouseButton) != 0) mask |= 0x04;
    return mask;
  }

  @override
  Widget build(BuildContext context) {
    return LayoutBuilder(
      builder: (context, constraints) {
        final w = constraints.maxWidth;
        final h = constraints.maxHeight;
        final cx = localCursorNorm.dx.clamp(0.0, 1.0) * w;
        final cy = localCursorNorm.dy.clamp(0.0, 1.0) * h;

        int normX(double dx) => ((dx / w).clamp(0.0, 1.0) * 65535).round();
        int normY(double dy) => ((dy / h).clamp(0.0, 1.0) * 65535).round();

        return Listener(
          onPointerHover: (ev) => onPointerEventNorm(
            normX(ev.localPosition.dx),
            normY(ev.localPosition.dy),
            0,
            0,
          ),
          onPointerMove: (ev) {
            if (ev.kind == PointerDeviceKind.mouse) {
              onPointerEventNorm(
                normX(ev.localPosition.dx),
                normY(ev.localPosition.dy),
                _mapButtons(ev.buttons),
                0,
              );
            }
          },
          onPointerDown: (ev) {
            if (ev.kind == PointerDeviceKind.mouse) {
              onPointerEventNorm(
                normX(ev.localPosition.dx),
                normY(ev.localPosition.dy),
                _mapButtons(ev.buttons),
                0,
              );
            }
          },
          onPointerUp: (ev) {
            if (ev.kind == PointerDeviceKind.mouse) {
              onPointerEventNorm(
                normX(ev.localPosition.dx),
                normY(ev.localPosition.dy),
                0,
                0,
              );
            }
          },
          onPointerSignal: (sig) {
            if (sig is PointerScrollEvent) {
              onPointerEventNorm(
                normX(sig.localPosition.dx),
                normY(sig.localPosition.dy),
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
              // Remote GPU frame surface
              Container(
                color: const Color(0xFF0E1117),
                child: frameImage != null
                    ? RawImage(image: frameImage, fit: BoxFit.contain)
                    : const Center(
                        child: Text(
                          '等待远端首帧画面 (静止画面 0 Kbps 智能休眠就绪)...',
                          style: TextStyle(color: Colors.white54, fontSize: 14),
                        ),
                      ),
              ),

              // Decoupled 0-latency local hardware-speed cursor overlay
              Positioned(
                left: cx,
                top: cy,
                child: IgnorePointer(
                  child: Icon(
                    Icons.near_me_rounded,
                    size: 20,
                    color: Colors.tealAccent.shade200,
                    shadows: const [Shadow(blurRadius: 4, color: Colors.black)],
                  ),
                ),
              ),

              // UU-Remote style Floating Telemetry Capsule HUD
              Positioned(
                top: 12,
                left: 16,
                child: Container(
                  padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 6),
                  decoration: BoxDecoration(
                    color: Colors.black.withValues(alpha: 0.72),
                    borderRadius: BorderRadius.circular(20),
                    border: Border.all(color: Colors.white12),
                  ),
                  child: Row(
                    mainAxisSize: MainAxisSize.min,
                    children: [
                      Icon(
                        Icons.bolt_rounded,
                        size: 16,
                        color: telemetry.transportPath == 1
                            ? Colors.greenAccent
                            : Colors.amberAccent,
                      ),
                      const SizedBox(width: 6),
                      Text(
                        '${telemetry.pathBadge}  |  '
                        'RTT ${telemetry.rttMs.toStringAsFixed(1)}ms  |  '
                        '${telemetry.formattedBandwidth}  |  '
                        '${telemetry.currentFps.toStringAsFixed(0)} FPS  |  '
                        '${telemetry.codecBadge}'
                        '${telemetry.fecRecoveredFrames > 0 ? "  |  FEC修复:${telemetry.fecRecoveredFrames}" : ""}',
                        style: const TextStyle(
                          color: Colors.white,
                          fontSize: 12,
                          fontFamily: 'monospace',
                        ),
                      ),
                    ],
                  ),
                ),
              ),
            ],
          ),
          ),
        );
      },
    );
  }
}
