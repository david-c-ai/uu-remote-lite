#include "dircore_ffi.h"
#include "session_engine.hpp"

#include <cstring>
#include <memory>

struct DirHandleImpl {
    std::unique_ptr<uu::core::PeerNode> node;
};

extern "C" {

DirHandle dir_node_create(const char* self_id,
                          const char* access_pin,
                          const char* server_ip,
                          uint16_t hbbs_port,
                          uint16_t hbbr_port,
                          int32_t is_host) {
    uu::core::NodeConfig cfg{};
    if (self_id && *self_id) cfg.self_id = self_id;
    if (access_pin) cfg.access_pin = access_pin;
    if (server_ip && *server_ip) cfg.server_ip = server_ip;
    if (hbbs_port != 0) cfg.hbbs_port = hbbs_port;
    if (hbbr_port != 0) cfg.hbbr_port = hbbr_port;
    cfg.is_host = (is_host != 0);

    auto* impl = new DirHandleImpl();
    impl->node = std::make_unique<uu::core::PeerNode>(cfg);
    return impl;
}

int32_t dir_node_start(DirHandle handle) {
    if (!handle || !handle->node) return 0;
    return handle->node->Start() ? 1 : 0;
}

int32_t dir_node_connect(DirHandle handle, const char* target_host_id, uint32_t timeout_ms) {
    if (!handle || !handle->node || !target_host_id) return 0;
    return handle->node->ConnectToPeer(target_host_id, timeout_ms) ? 1 : 0;
}

void dir_node_stop(DirHandle handle) {
    if (handle && handle->node) handle->node->Stop();
}

void dir_node_destroy(DirHandle handle) {
    if (!handle) return;
    if (handle->node) handle->node->Stop();
    delete handle;
}

void dir_node_get_telemetry(DirHandle handle, DirTelemetryC* out_telemetry) {
    if (!handle || !handle->node || !out_telemetry) return;
    std::memset(out_telemetry, 0, sizeof(DirTelemetryC));
    auto t = handle->node->GetTelemetry();
    out_telemetry->transport_path = static_cast<uint8_t>(t.path);
    out_telemetry->active_profile = static_cast<uint8_t>(t.active_profile);
    out_telemetry->active_codec = static_cast<uint8_t>(t.active_codec);
    out_telemetry->privacy_curtain = t.privacy_curtain_on ? 1 : 0;
    out_telemetry->monitor_count = t.monitor_count > 0 ? t.monitor_count : 1;
    out_telemetry->active_monitor = t.active_monitor;
    out_telemetry->connect_time_ms = t.connect_time_ms;
    out_telemetry->rtt_ms = t.rtt_ms;
    out_telemetry->loss_pct = t.loss_pct;
    out_telemetry->realtime_kbps = t.realtime_kbps;
    out_telemetry->current_fps = t.current_fps;
    out_telemetry->avg_encode_ms = t.avg_encode_ms;
    out_telemetry->avg_decode_ms = t.avg_decode_ms;
    out_telemetry->frames_decoded = t.frames_decoded;
    out_telemetry->fec_recovered_frames = t.fec_recovered_frames;
    out_telemetry->remote_width = t.remote_width;
    out_telemetry->remote_height = t.remote_height;
    out_telemetry->cursor_x_norm = t.cursor_x_norm;
    out_telemetry->cursor_y_norm = t.cursor_y_norm;
}

int32_t dir_node_copy_frame_rgba(DirHandle handle,
                                 uint8_t* dst_rgba_buf,
                                 uint32_t dst_capacity,
                                 uint16_t* out_width,
                                 uint16_t* out_height) {
    if (!handle || !handle->node || !dst_rgba_buf) return 0;
    std::vector<uint8_t> bgra;
    uint16_t w = 0, h = 0;
    if (!handle->node->CopyLatestFrameBgra(bgra, w, h)) return 0;
    const size_t expected = static_cast<size_t>(w) * h * 4;
    if (bgra.size() < expected || expected == 0) return 0;
    if (out_width) *out_width = w;
    if (out_height) *out_height = h;
    if (expected > dst_capacity) return -1;

    // Auto-vectorized BGRA8888 -> RGBA8888 conversion for universal GPU rendering
    const uint8_t* src = bgra.data();
    for (size_t i = 0; i < expected; i += 4) {
        dst_rgba_buf[i + 0] = src[i + 2]; // R
        dst_rgba_buf[i + 1] = src[i + 1]; // G
        dst_rgba_buf[i + 2] = src[i + 0]; // B
        dst_rgba_buf[i + 3] = 255;        // Force opaque alpha
    }
    return static_cast<int32_t>(expected);
}

int32_t dir_node_copy_frame_ex(DirHandle handle,
                               uint64_t last_seen_seq,
                               uint64_t* out_seq,
                               uint8_t* out_format,
                               uint8_t* dst_buf,
                               uint32_t dst_capacity,
                               uint16_t* out_width,
                               uint16_t* out_height) {
    if (!handle || !handle->node || !dst_buf) return 0;
    uint64_t seq = 0;
    uint8_t fmt = 0;
    std::vector<uint8_t> raw;
    uint16_t w = 0, h = 0;
    if (!handle->node->CopyLatestFrameEx(last_seen_seq, seq, fmt, raw, w, h)) return 0;

    if (out_seq) *out_seq = seq;
    if (out_width) *out_width = w;
    if (out_height) *out_height = h;

    if (fmt == 2) {
        // Direct compact JPEG bitstream (~40-70KB): zero CPU conversion!
        if (raw.size() > dst_capacity) return -1;
        if (out_format) *out_format = 2;
        std::memcpy(dst_buf, raw.data(), raw.size());
        return static_cast<int32_t>(raw.size());
    }

    const size_t expected = static_cast<size_t>(w) * h * 4;
    if (raw.size() < expected || expected == 0 || expected > dst_capacity) return -1;
    if (out_format) *out_format = 1;
    const uint8_t* src = raw.data();
    for (size_t i = 0; i < expected; i += 4) {
        dst_buf[i + 0] = src[i + 2];
        dst_buf[i + 1] = src[i + 1];
        dst_buf[i + 2] = src[i + 0];
        dst_buf[i + 3] = 255;
    }
    return static_cast<int32_t>(expected);
}

void dir_node_send_mouse_ex(DirHandle handle, uint16_t x_norm, uint16_t y_norm,
                            int16_t dx, int16_t dy, uint8_t button_mask, int16_t wheel_y) {
    if (handle && handle->node) {
        handle->node->SendCursorMoveFast(x_norm, y_norm, dx, dy, button_mask, wheel_y);
    }
}

void dir_node_send_key(DirHandle handle, uint16_t keycode, int32_t is_down, uint8_t modifiers) {
    if (handle && handle->node) handle->node->SendKey(keycode, is_down != 0, modifiers);
}

void dir_node_send_gamepad(DirHandle handle, uint16_t buttons,
                           int16_t lx, int16_t ly, int16_t rx, int16_t ry,
                           uint8_t lt, uint8_t rt) {
    if (!handle || !handle->node) return;
    uu::core::PayloadGamepad pad{buttons, lx, ly, rx, ry, lt, rt};
    handle->node->SendGamepad(pad);
}

void dir_node_set_options(DirHandle handle, uint8_t profile, uint8_t target_fps,
                          int32_t privacy_curtain, uint8_t monitor_index) {
    if (!handle || !handle->node) return;
    auto qp = static_cast<uu::core::QualityProfile>(profile <= 2 ? profile : 1);
    handle->node->SetQualityProfile(qp, target_fps, privacy_curtain != 0, monitor_index);
}

void dir_node_send_clipboard(DirHandle handle, const char* utf8_text) {
    if (handle && handle->node && utf8_text) {
        handle->node->SendClipboardText(utf8_text);
    }
}

int32_t dir_node_send_file(DirHandle handle, const char* local_path, uint64_t file_id) {
    if (!handle || !handle->node || !local_path) return 0;
    return handle->node->SendFile(local_path, file_id) ? 1 : 0;
}

} // extern "C"
