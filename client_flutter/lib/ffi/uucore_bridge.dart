import 'dart:ffi' as ffi;
import 'dart:io';
import 'dart:typed_data';
import 'package:ffi/ffi.dart';
import '../models/session_models.dart';

final class UUTelemetryC extends ffi.Struct {
  @ffi.Uint8()
  external int transportPath;
  @ffi.Uint8()
  external int activeProfile;
  @ffi.Uint8()
  external int activeCodec;
  @ffi.Uint8()
  external int privacyCurtain;

  @ffi.Float()
  external double connectTimeMs;
  @ffi.Float()
  external double rttMs;
  @ffi.Float()
  external double lossPct;
  @ffi.Float()
  external double realtimeKbps;
  @ffi.Float()
  external double currentFps;
  @ffi.Float()
  external double avgEncodeMs;
  @ffi.Float()
  external double avgDecodeMs;

  @ffi.Uint64()
  external int framesDecoded;
  @ffi.Uint64()
  external int fecRecoveredFrames;

  @ffi.Uint16()
  external int remoteWidth;
  @ffi.Uint16()
  external int remoteHeight;
  @ffi.Uint16()
  external int cursorXNorm;
  @ffi.Uint16()
  external int cursorYNorm;
}

typedef _CreateNative = ffi.Pointer<ffi.Void> Function(
    ffi.Pointer<Utf8>, ffi.Pointer<Utf8>, ffi.Pointer<Utf8>, ffi.Uint16, ffi.Uint16, ffi.Int32);
typedef _CreateDart = ffi.Pointer<ffi.Void> Function(
    ffi.Pointer<Utf8>, ffi.Pointer<Utf8>, ffi.Pointer<Utf8>, int, int, int);

typedef _StartNative = ffi.Int32 Function(ffi.Pointer<ffi.Void>);
typedef _StartDart = int Function(ffi.Pointer<ffi.Void>);

typedef _ConnectNative = ffi.Int32 Function(ffi.Pointer<ffi.Void>, ffi.Pointer<Utf8>, ffi.Uint32);
typedef _ConnectDart = int Function(ffi.Pointer<ffi.Void>, ffi.Pointer<Utf8>, int);

typedef _VoidHandleNative = ffi.Void Function(ffi.Pointer<ffi.Void>);
typedef _VoidHandleDart = void Function(ffi.Pointer<ffi.Void>);

typedef _GetTelemetryNative = ffi.Void Function(ffi.Pointer<ffi.Void>, ffi.Pointer<UUTelemetryC>);
typedef _GetTelemetryDart = void Function(ffi.Pointer<ffi.Void>, ffi.Pointer<UUTelemetryC>);

typedef _CopyFrameNative = ffi.Int32 Function(
    ffi.Pointer<ffi.Void>, ffi.Pointer<ffi.Uint8>, ffi.Uint32, ffi.Pointer<ffi.Uint16>, ffi.Pointer<ffi.Uint16>);
typedef _CopyFrameDart = int Function(
    ffi.Pointer<ffi.Void>, ffi.Pointer<ffi.Uint8>, int, ffi.Pointer<ffi.Uint16>, ffi.Pointer<ffi.Uint16>);

typedef _SendMouseExNative = ffi.Void Function(
    ffi.Pointer<ffi.Void>, ffi.Uint16, ffi.Uint16, ffi.Int16, ffi.Int16, ffi.Uint8, ffi.Int16);
typedef _SendMouseExDart = void Function(ffi.Pointer<ffi.Void>, int, int, int, int, int, int);

typedef _SendKeyNative = ffi.Void Function(ffi.Pointer<ffi.Void>, ffi.Uint16, ffi.Int32, ffi.Uint8);
typedef _SendKeyDart = void Function(ffi.Pointer<ffi.Void>, int, int, int);

typedef _SendGamepadNative = ffi.Void Function(
    ffi.Pointer<ffi.Void>, ffi.Uint16, ffi.Int16, ffi.Int16, ffi.Int16, ffi.Int16, ffi.Uint8, ffi.Uint8);
typedef _SendGamepadDart = void Function(ffi.Pointer<ffi.Void>, int, int, int, int, int, int, int);

typedef _SetQualityNative = ffi.Void Function(ffi.Pointer<ffi.Void>, ffi.Uint8, ffi.Uint8, ffi.Int32);
typedef _SetQualityDart = void Function(ffi.Pointer<ffi.Void>, int, int, int);

typedef _SendClipboardNative = ffi.Void Function(ffi.Pointer<ffi.Void>, ffi.Pointer<Utf8>);
typedef _SendClipboardDart = void Function(ffi.Pointer<ffi.Void>, ffi.Pointer<Utf8>);

typedef _SendFileNative = ffi.Int32 Function(ffi.Pointer<ffi.Void>, ffi.Pointer<Utf8>, ffi.Uint64);
typedef _SendFileDart = int Function(ffi.Pointer<ffi.Void>, ffi.Pointer<Utf8>, int);

class UUCoreBridge {
  UUCoreBridge._(this._lib) {
    _create = _lib.lookupFunction<_CreateNative, _CreateDart>('uu_node_create');
    _start = _lib.lookupFunction<_StartNative, _StartDart>('uu_node_start');
    _connect = _lib.lookupFunction<_ConnectNative, _ConnectDart>('uu_node_connect');
    _stop = _lib.lookupFunction<_VoidHandleNative, _VoidHandleDart>('uu_node_stop');
    _destroy = _lib.lookupFunction<_VoidHandleNative, _VoidHandleDart>('uu_node_destroy');
    _getTelemetry = _lib.lookupFunction<_GetTelemetryNative, _GetTelemetryDart>('uu_node_get_telemetry');
    _copyFrame = _lib.lookupFunction<_CopyFrameNative, _CopyFrameDart>('uu_node_copy_frame_bgra');
    _sendMouseEx = _lib.lookupFunction<_SendMouseExNative, _SendMouseExDart>('uu_node_send_mouse_ex');
    _sendKey = _lib.lookupFunction<_SendKeyNative, _SendKeyDart>('uu_node_send_key');
    _sendGamepad = _lib.lookupFunction<_SendGamepadNative, _SendGamepadDart>('uu_node_send_gamepad');
    _setQuality = _lib.lookupFunction<_SetQualityNative, _SetQualityDart>('uu_node_set_quality');
    _sendClipboard = _lib.lookupFunction<_SendClipboardNative, _SendClipboardDart>('uu_node_send_clipboard');
    _sendFile = _lib.lookupFunction<_SendFileNative, _SendFileDart>('uu_node_send_file');
  }

  static UUCoreBridge? tryLoad() {
    final exeDir = File(Platform.resolvedExecutable).parent.path;
    final candidates = <String>[
      if (Platform.isMacOS) ...[
        '$exeDir/../Frameworks/libuucore.dylib',
        '$exeDir/libuucore.dylib',
        'libuucore.dylib',
        '../build/libuucore.dylib',
      ] else if (Platform.isWindows) ...[
        '$exeDir\\uucore.dll',
        'uucore.dll',
      ] else if (!Platform.isIOS) ...[
        'libuucore.so',
      ]
    ];

    if (Platform.isIOS) {
      try {
        return UUCoreBridge._(ffi.DynamicLibrary.process());
      } catch (_) {
        return null;
      }
    }

    for (final path in candidates) {
      try {
        return UUCoreBridge._(ffi.DynamicLibrary.open(path));
      } catch (_) {
        continue;
      }
    }
    return null;
  }

  final ffi.DynamicLibrary _lib;
  late final _CreateDart _create;
  late final _StartDart _start;
  late final _ConnectDart _connect;
  late final _VoidHandleDart _stop;
  late final _VoidHandleDart _destroy;
  late final _GetTelemetryDart _getTelemetry;
  late final _CopyFrameDart _copyFrame;
  late final _SendMouseExDart _sendMouseEx;
  late final _SendKeyDart _sendKey;
  late final _SendGamepadDart _sendGamepad;
  late final _SetQualityDart _setQuality;
  late final _SendClipboardDart _sendClipboard;
  late final _SendFileDart _sendFile;

  ffi.Pointer<ffi.Void> _handle = ffi.nullptr;
  ffi.Pointer<ffi.Uint8> _frameBuf = ffi.nullptr;
  int _frameCap = 0;

  bool startSession({
    required String selfId,
    required String accessPin,
    required String serverIp,
    int hbbsPort = 21116,
    int hbbrPort = 21117,
    bool isHost = false,
  }) {
    dispose();
    final idPtr = selfId.toNativeUtf8();
    final pinPtr = accessPin.toNativeUtf8();
    final ipPtr = serverIp.toNativeUtf8();
    try {
      _handle = _create(idPtr, pinPtr, ipPtr, hbbsPort, hbbrPort, isHost ? 1 : 0);
      if (_handle == ffi.nullptr) return false;
      return _start(_handle) == 1;
    } finally {
      calloc.free(idPtr);
      calloc.free(pinPtr);
      calloc.free(ipPtr);
    }
  }

  bool connectToHost(String targetHostId, {int timeoutMs = 2500}) {
    if (_handle == ffi.nullptr) return false;
    final targetPtr = targetHostId.toNativeUtf8();
    try {
      return _connect(_handle, targetPtr, timeoutMs) == 1;
    } finally {
      calloc.free(targetPtr);
    }
  }

  TelemetrySnapshot pollTelemetry() {
    if (_handle == ffi.nullptr) return const TelemetrySnapshot();
    final ptr = calloc<UUTelemetryC>();
    try {
      _getTelemetry(_handle, ptr);
      final ref = ptr.ref;
      return TelemetrySnapshot(
        transportPath: ref.transportPath,
        activeProfile: ref.activeProfile,
        activeCodec: ref.activeCodec,
        privacyCurtain: ref.privacyCurtain != 0,
        connectTimeMs: ref.connectTimeMs,
        rttMs: ref.rttMs,
        lossPct: ref.lossPct,
        realtimeKbps: ref.realtimeKbps,
        currentFps: ref.currentFps,
        avgEncodeMs: ref.avgEncodeMs,
        avgDecodeMs: ref.avgDecodeMs,
        framesDecoded: ref.framesDecoded,
        fecRecoveredFrames: ref.fecRecoveredFrames,
        remoteWidth: ref.remoteWidth,
        remoteHeight: ref.remoteHeight,
        cursorXNorm: ref.cursorXNorm,
        cursorYNorm: ref.cursorYNorm,
      );
    } finally {
      calloc.free(ptr);
    }
  }

  ({Uint8List bytes, int width, int height})? copyLatestFrame() {
    if (_handle == ffi.nullptr) return null;
    const maxCap = 3840 * 2160 * 4;
    if (_frameBuf == ffi.nullptr || _frameCap < maxCap) {
      if (_frameBuf != ffi.nullptr) calloc.free(_frameBuf);
      _frameBuf = calloc<ffi.Uint8>(maxCap);
      _frameCap = maxCap;
    }
    final wPtr = calloc<ffi.Uint16>();
    final hPtr = calloc<ffi.Uint16>();
    try {
      final copied = _copyFrame(_handle, _frameBuf, _frameCap, wPtr, hPtr);
      if (copied <= 0) return null;
      return (
        bytes: Uint8List.fromList(_frameBuf.asTypedList(copied)),
        width: wPtr.value,
        height: hPtr.value,
      );
    } finally {
      calloc.free(wPtr);
      calloc.free(hPtr);
    }
  }

  void sendMouse(
    int xNorm,
    int yNorm, {
    int dx = 0,
    int dy = 0,
    int buttonMask = 0,
    int wheelY = 0,
  }) {
    if (_handle != ffi.nullptr) {
      _sendMouseEx(
        _handle,
        xNorm.clamp(0, 65535),
        yNorm.clamp(0, 65535),
        dx,
        dy,
        buttonMask,
        wheelY,
      );
    }
  }

  void sendKey(int keycode, {required bool isDown, int modifiers = 0}) {
    if (_handle != ffi.nullptr) {
      _sendKey(_handle, keycode, isDown ? 1 : 0, modifiers);
    }
  }

  void sendGamepad({
    required int buttons,
    int lx = 0,
    int ly = 0,
    int rx = 0,
    int ry = 0,
    int lt = 0,
    int rt = 0,
  }) {
    if (_handle != ffi.nullptr) {
      _sendGamepad(_handle, buttons, lx, ly, rx, ry, lt, rt);
    }
  }

  void setQuality(QualityPreset preset, {required bool privacyCurtain}) {
    if (_handle != ffi.nullptr) {
      _setQuality(_handle, preset.code, preset.defaultFps, privacyCurtain ? 1 : 0);
    }
  }

  void sendClipboard(String text) {
    if (_handle == ffi.nullptr) return;
    final p = text.toNativeUtf8();
    try {
      _sendClipboard(_handle, p);
    } finally {
      calloc.free(p);
    }
  }

  bool sendFile(String path, {int fileId = 1}) {
    if (_handle == ffi.nullptr) return false;
    final p = path.toNativeUtf8();
    try {
      return _sendFile(_handle, p, fileId) == 1;
    } finally {
      calloc.free(p);
    }
  }

  void dispose() {
    if (_handle != ffi.nullptr) {
      _stop(_handle);
      _destroy(_handle);
      _handle = ffi.nullptr;
    }
    if (_frameBuf != ffi.nullptr) {
      calloc.free(_frameBuf);
      _frameBuf = ffi.nullptr;
      _frameCap = 0;
    }
  }
}
