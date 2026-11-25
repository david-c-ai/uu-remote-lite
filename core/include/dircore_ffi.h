#pragma once

#include <stdint.h>

#if defined(_WIN32)
  #if defined(dircore_EXPORTS)
    #define DIRCORE_API __declspec(dllexport)
  #else
    #define DIRCORE_API __declspec(dllimport)
  #endif
#else
  #define DIRCORE_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DirHandleImpl* DirHandle;

typedef struct DirTelemetryC {
    uint8_t transport_path;   // 0 = Disconnected, 1 = P2P Direct, 2 = Relay Fallback
    uint8_t active_profile;   // 0 = Saver 300Kbps, 1 = Balanced Auto, 2 = Gaming 144Hz
    uint8_t active_codec;     // 1 = TileDeltaLZ4, 2 = Hardware HEVC
    uint8_t privacy_curtain;  // 0 = Off, 1 = On
    uint8_t monitor_count;    // Host display count (1..16)
    uint8_t active_monitor;   // Currently selected host display index (0..N-1)
    uint16_t reserved_pad;    // Explicit 8-byte alignment before floats
    float connect_time_ms;
    float rtt_ms;
    float loss_pct;
    float realtime_kbps;
    float current_fps;
    float avg_encode_ms;
    float avg_decode_ms;
    float reserved_f;         // Explicit 8-byte alignment before uint64_t
    uint64_t frames_decoded;
    uint64_t fec_recovered_frames;
    uint16_t remote_width;
    uint16_t remote_height;
    uint16_t cursor_x_norm;
    uint16_t cursor_y_norm;
} DirTelemetryC;

// Lifecycle & Connection
DIRCORE_API DirHandle dir_node_create(const char* self_id,
                                      const char* access_pin,
                                      const char* server_ip,
                                      uint16_t hbbs_port,
                                      uint16_t hbbr_port,
                                      int32_t is_host);

DIRCORE_API int32_t dir_node_start(DirHandle handle);
DIRCORE_API int32_t dir_node_connect(DirHandle handle, const char* target_host_id, uint32_t timeout_ms);
DIRCORE_API void dir_node_stop(DirHandle handle);
DIRCORE_API void dir_node_destroy(DirHandle handle);

// Telemetry & RGBA8888 Frame Copy (for universal Flutter ui.PixelFormat.rgba8888 rendering)
DIRCORE_API void dir_node_get_telemetry(DirHandle handle, DirTelemetryC* out_telemetry);
DIRCORE_API int32_t dir_node_copy_frame_rgba(DirHandle handle,
                                             uint8_t* dst_rgba_buf,
                                             uint32_t dst_capacity,
                                             uint16_t* out_width,
                                             uint16_t* out_height);

// Zero-redundant-copy frame fetch:
// Returns 0 if no new frame since last_seen_seq (0 CPU on static desktop!).
// out_format: 1 = RGBA8888 raw, 2 = JPEG bitstream (for Flutter ui.instantiateImageCodec libjpeg-turbo!)
DIRCORE_API int32_t dir_node_copy_frame_ex(DirHandle handle,
                                           uint64_t last_seen_seq,
                                           uint64_t* out_seq,
                                           uint8_t* out_format,
                                           uint8_t* dst_buf,
                                           uint32_t dst_capacity,
                                           uint16_t* out_width,
                                           uint16_t* out_height);

// Input, Multi-Monitor Selection & Controls
DIRCORE_API void dir_node_send_mouse_ex(DirHandle handle, uint16_t x_norm, uint16_t y_norm,
                                        int16_t dx, int16_t dy, uint8_t button_mask, int16_t wheel_y);
DIRCORE_API void dir_node_send_key(DirHandle handle, uint16_t keycode, int32_t is_down, uint8_t modifiers);
DIRCORE_API void dir_node_send_gamepad(DirHandle handle, uint16_t buttons,
                                       int16_t lx, int16_t ly, int16_t rx, int16_t ry,
                                       uint8_t lt, uint8_t rt);
DIRCORE_API void dir_node_set_options(DirHandle handle, uint8_t profile, uint8_t target_fps,
                                      int32_t privacy_curtain, uint8_t monitor_index);
DIRCORE_API void dir_node_send_clipboard(DirHandle handle, const char* utf8_text);
DIRCORE_API int32_t dir_node_send_file(DirHandle handle, const char* local_path, uint64_t file_id);

#ifdef __cplusplus
}
#endif
