#pragma once

#include <stdint.h>

#if defined(_WIN32)
#define UUCORE_API __declspec(dllexport)
#else
#define UUCORE_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct UUHandleImpl* UUHandle;

typedef struct UUTelemetryC {
    uint8_t transport_path;   // 0 = Disconnected, 1 = P2P Direct, 2 = Relay Fallback
    uint8_t active_profile;   // 0 = Saver 300Kbps, 1 = Balanced Auto, 2 = Gaming 144Hz
    uint8_t active_codec;     // 1 = TileDeltaLZ4, 2 = Hardware HEVC
    uint8_t privacy_curtain;  // 0 = Off, 1 = On
    float connect_time_ms;
    float rtt_ms;
    float loss_pct;
    float realtime_kbps;
    float current_fps;
    float avg_encode_ms;
    float avg_decode_ms;
    uint64_t frames_decoded;
    uint64_t fec_recovered_frames;
    uint16_t remote_width;
    uint16_t remote_height;
    uint16_t cursor_x_norm;
    uint16_t cursor_y_norm;
} UUTelemetryC;

// Lifecycle & Connection
UUCORE_API UUHandle uu_node_create(const char* self_id,
                                   const char* access_pin,
                                   const char* server_ip,
                                   uint16_t hbbs_port,
                                   uint16_t hbbr_port,
                                   int32_t is_host);

UUCORE_API int32_t uu_node_start(UUHandle handle);
UUCORE_API int32_t uu_node_connect(UUHandle handle, const char* target_host_id, uint32_t timeout_ms);
UUCORE_API void uu_node_stop(UUHandle handle);
UUCORE_API void uu_node_destroy(UUHandle handle);

// Telemetry & Frame Copy (for Flutter Texture / PixelBuffer rendering)
UUCORE_API void uu_node_get_telemetry(UUHandle handle, UUTelemetryC* out_telemetry);
UUCORE_API int32_t uu_node_copy_frame_bgra(UUHandle handle,
                                           uint8_t* dst_buf,
                                           uint32_t dst_capacity,
                                           uint16_t* out_width,
                                           uint16_t* out_height);

// Low-Latency Input & Full-Featured Controls
UUCORE_API void uu_node_send_mouse(UUHandle handle, uint16_t x_norm, uint16_t y_norm, int16_t dx, int16_t dy);
UUCORE_API void uu_node_send_mouse_ex(UUHandle handle, uint16_t x_norm, uint16_t y_norm,
                                      int16_t dx, int16_t dy, uint8_t button_mask, int16_t wheel_y);
UUCORE_API void uu_node_send_key(UUHandle handle, uint16_t keycode, int32_t is_down, uint8_t modifiers);
UUCORE_API void uu_node_send_gamepad(UUHandle handle, uint16_t buttons,
                                     int16_t lx, int16_t ly, int16_t rx, int16_t ry,
                                     uint8_t lt, uint8_t rt);
UUCORE_API void uu_node_set_quality(UUHandle handle, uint8_t profile, uint8_t target_fps, int32_t privacy_curtain);
UUCORE_API void uu_node_send_clipboard(UUHandle handle, const char* utf8_text);
UUCORE_API int32_t uu_node_send_file(UUHandle handle, const char* local_path, uint64_t file_id);

#ifdef __cplusplus
}
#endif
