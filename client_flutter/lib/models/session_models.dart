enum QualityPreset {
  saver300k(0, '极速省流', '300Kbps 窄带', 30),
  balancedAuto(1, '智能均衡', '自适应帧率', 60),
  gaming144hz(2, '电竞高刷', '144Hz 原画', 144);

  const QualityPreset(this.code, this.shortLabel, this.subtitle, this.defaultFps);
  final int code;
  final String shortLabel;
  final String subtitle;
  final int defaultFps;
}

enum MobileInputMode {
  trackpadRelative('触控板'),
  directTouchZoom('直触/缩放'),
  virtualGamepad('虚拟手柄');

  const MobileInputMode(this.label);
  final String label;
}

enum ScreenOrientationMode {
  autoRotate('自动旋转'),
  landscape('锁定横屏'),
  portrait('锁定竖屏');

  const ScreenOrientationMode(this.label);
  final String label;
}

enum ViewportScaleMode {
  contain('等比完整'),
  fill('铺满全屏');

  const ViewportScaleMode(this.label);
  final String label;
}

class TelemetrySnapshot {
  const TelemetrySnapshot({
    this.transportPath = 0,
    this.activeProfile = 1,
    this.activeCodec = 1,
    this.privacyCurtain = false,
    this.monitorCount = 1,
    this.activeMonitor = 0,
    this.connectTimeMs = 0,
    this.rttMs = 0,
    this.lossPct = 0,
    this.realtimeKbps = 0,
    this.currentFps = 0,
    this.avgEncodeMs = 0,
    this.avgDecodeMs = 0,
    this.framesDecoded = 0,
    this.fecRecoveredFrames = 0,
    this.remoteWidth = 1280,
    this.remoteHeight = 720,
    this.cursorXNorm = 32768,
    this.cursorYNorm = 32768,
  });

  final int transportPath;
  final int activeProfile;
  final int activeCodec;
  final bool privacyCurtain;
  final int monitorCount;
  final int activeMonitor;
  final double connectTimeMs;
  final double rttMs;
  final double lossPct;
  final double realtimeKbps;
  final double currentFps;
  final double avgEncodeMs;
  final double avgDecodeMs;
  final int framesDecoded;
  final int fecRecoveredFrames;
  final int remoteWidth;
  final int remoteHeight;
  final int cursorXNorm;
  final int cursorYNorm;

  String get pathBadge => switch (transportPath) {
        1 => 'P2P 直连',
        2 => 'Relay 中继',
        _ => '重连中',
      };

  String get codecBadge => switch (activeCodec) {
        4 => 'JPEG 极速',
        2 => 'HEVC 硬编',
        _ => 'Tile 窄带',
      };

  String get formattedBandwidth => realtimeKbps >= 1024
      ? '${(realtimeKbps / 1024.0).toStringAsFixed(2)} Mbps'
      : '${realtimeKbps.toStringAsFixed(0)} Kbps';
}
