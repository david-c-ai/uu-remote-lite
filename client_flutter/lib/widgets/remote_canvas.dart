import 'dart:ui' as ui;
import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import '../models/session_models.dart';

class RemoteCanvas extends StatefulWidget {
  const RemoteCanvas({
    super.key,
    required this.frameImage,
    required this.telemetry,
    required this.localCursorNorm,
    required this.scaleMode,
    required this.enablePinchZoom,
    required this.onPointerEventNorm,
  });

  final ui.Image? frameImage;
  final TelemetrySnapshot telemetry;
  final Offset localCursorNorm;
  final ViewportScaleMode scaleMode;
  final bool enablePinchZoom;
  final void Function(int xNorm, int yNorm, int buttonMask, int wheelY) onPointerEventNorm;

  @override
  State<RemoteCanvas> createState() => _RemoteCanvasState();
}

class _RemoteCanvasState extends State<RemoteCanvas> {
  bool _expandedHud = false;
  final TransformationController _zoomCtrl = TransformationController();

  @override
  void dispose() {
    _zoomCtrl.dispose();
    super.dispose();
  }

  int _mapButtons(int flutterButtons) {
    int mask = 0;
    if ((flutterButtons & kPrimaryMouseButton) != 0) mask |= 0x01;
    if ((flutterButtons & kSecondaryMouseButton) != 0) mask |= 0x02;
    if ((flutterButtons & kMiddleMouseButton) != 0) mask |= 0x04;
    return mask;
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
    return LayoutBuilder(
      builder: (context, constraints) {
        final viewport = Size(constraints.maxWidth, constraints.maxHeight);
        final rect = _computeDisplayRect(viewport);

        int normX(double dx) =>
            (((dx - rect.left) / (rect.width > 1 ? rect.width : 1)).clamp(0.0, 1.0) * 65535).round();
        int normY(double dy) =>
            (((dy - rect.top) / (rect.height > 1 ? rect.height : 1)).clamp(0.0, 1.0) * 65535).round();

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

            // Decoupled local hardware-speed cursor overlay
            Positioned(
              left: cx,
              top: cy,
              child: IgnorePointer(
                child: Icon(
                  Icons.near_me_rounded,
                  size: 19,
                  color: const Color(0xFF4FFFB0),
                  shadows: const [Shadow(blurRadius: 5, color: Colors.black)],
                ),
              ),
            ),
          ],
        );

        if (widget.enablePinchZoom) {
          videoSurface = InteractiveViewer(
            transformationController: _zoomCtrl,
            minScale: 1.0,
            maxScale: 4.5,
            panEnabled: true,
            scaleEnabled: true,
            child: videoSurface,
          );
        }

        final tele = widget.telemetry;
        final monText = tele.monitorCount > 1
            ? '  ·  屏${tele.activeMonitor + 1}/${tele.monitorCount}'
            : '';

        return Listener(
          onPointerHover: (ev) => widget.onPointerEventNorm(
            normX(ev.localPosition.dx),
            normY(ev.localPosition.dy),
            0,
            0,
          ),
          onPointerMove: (ev) {
            if (ev.kind == PointerDeviceKind.mouse) {
              widget.onPointerEventNorm(
                normX(ev.localPosition.dx),
                normY(ev.localPosition.dy),
                _mapButtons(ev.buttons),
                0,
              );
            }
          },
          onPointerDown: (ev) {
            if (ev.kind == PointerDeviceKind.mouse) {
              widget.onPointerEventNorm(
                normX(ev.localPosition.dx),
                normY(ev.localPosition.dy),
                _mapButtons(ev.buttons),
                0,
              );
            }
          },
          onPointerUp: (ev) {
            if (ev.kind == PointerDeviceKind.mouse) {
              widget.onPointerEventNorm(
                normX(ev.localPosition.dx),
                normY(ev.localPosition.dy),
                0,
                0,
              );
            }
          },
          onPointerSignal: (sig) {
            if (sig is PointerScrollEvent) {
              widget.onPointerEventNorm(
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
                                    ? '${tele.pathBadge} · ${tele.rttMs.toStringAsFixed(1)}ms · '
                                        '${tele.formattedBandwidth} · ${tele.currentFps.toStringAsFixed(0)}fps · '
                                        '${tele.remoteWidth}x${tele.remoteHeight}$monText'
                                    : '${tele.pathBadge} · ${tele.rttMs.toStringAsFixed(0)}ms · ${tele.formattedBandwidth}$monText',
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
    );
  }
}
