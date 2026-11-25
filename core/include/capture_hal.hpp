#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace uu::core {

struct MonitorInfo {
    uint32_t index = 0;
    uint32_t os_display_id = 0;
    int32_t origin_x = 0;
    int32_t origin_y = 0;
    uint16_t width = 1920;
    uint16_t height = 1080;
    uint16_t refresh_hz = 60;
    bool is_primary = true;
    std::string name;
};

struct DirtyTileRect {
    uint16_t x = 0;
    uint16_t y = 0;
    uint16_t w = 0;
    uint16_t h = 0;
};

struct CursorSnapshot {
    uint16_t x_norm = 32768; // 0..65535
    uint16_t y_norm = 32768;
    uint64_t shape_hash = 0;
    bool shape_changed = false;
    uint16_t width = 0;
    uint16_t height = 0;
    uint16_t hotspot_x = 0;
    uint16_t hotspot_y = 0;
    std::vector<uint8_t> bgra_pixels; // Populated only when shape_changed == true
};

struct CapturedFrame {
    uint32_t frame_id = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    uint32_t stride_bytes = 0;
    uint64_t capture_ts_us = 0;
    bool is_static_idle = false; // True when 0 pixels changed -> 0 Kbps video output!
    float dirty_ratio = 1.0f;    // 0.0 ~ 1.0 ratio of changed 64x64 tiles
    std::vector<DirtyTileRect> dirty_tiles;
    std::vector<uint8_t> bgra;   // Tightly packed or stride-aligned BGRA8888 buffer
};

// Fast 64x64 tile dirty-rectangle analyzer (SIMD-friendly 64-bit word scan)
class DirtyRectTracker {
public:
    static constexpr uint16_t kTileSize = 64;

    void Reset() { prev_bgra_.clear(); prev_w_ = 0; prev_h_ = 0; }

    // Compares current frame with previous frame, populates dirty_tiles & dirty_ratio,
    // and updates internal reference buffer.
    void Analyze(CapturedFrame& frame, bool force_full_frame = false);

private:
    uint16_t prev_w_ = 0;
    uint16_t prev_h_ = 0;
    std::vector<uint8_t> prev_bgra_;
};

class IScreenCapturer {
public:
    virtual ~IScreenCapturer() = default;
    virtual std::vector<MonitorInfo> EnumerateMonitors() = 0;
    virtual bool SelectMonitor(uint32_t monitor_index) = 0;
    virtual MonitorInfo CurrentMonitor() const = 0;
    virtual uint8_t MonitorCount() = 0;
    virtual bool CaptureNext(CapturedFrame& out_frame, bool force_keyframe) = 0;
    virtual CursorSnapshot CaptureCursor() = 0;
};

// Creates OS-native screen capturer (macOS CoreGraphics/ScreenCapture, Windows DXGI Desktop Duplication)
std::unique_ptr<IScreenCapturer> CreateNativeCapturer();

// Creates deterministic synthetic desktop workload capturer (for headless testing & narrowband benchmarks)
enum class SyntheticWorkload {
    kStaticIdle,       // 0% dirty tiles -> tests 0 Kbps idle bandwidth
    kCodeEditorTyping, // ~3% dirty tiles (small active typing box + moving cursor) -> tests <300 Kbps narrowband
    kFullMotionGaming, // 100% dynamic gradient motion -> tests H.265/HEVC high-speed hardware pipeline
};

class SyntheticCapturer : public IScreenCapturer {
public:
    SyntheticCapturer(uint16_t width = 1280, uint16_t height = 720);
    void SetWorkload(SyntheticWorkload mode) { workload_ = mode; }

    std::vector<MonitorInfo> EnumerateMonitors() override;
    bool SelectMonitor(uint32_t monitor_index) override;
    MonitorInfo CurrentMonitor() const override;
    uint8_t MonitorCount() override { return 2; }
    bool CaptureNext(CapturedFrame& out_frame, bool force_keyframe) override;
    CursorSnapshot CaptureCursor() override;

private:
    uint16_t width_;
    uint16_t height_;
    uint32_t active_mon_ = 0;
    uint32_t seq_ = 0;
    uint64_t last_cursor_hash_ = 0;
    SyntheticWorkload workload_ = SyntheticWorkload::kCodeEditorTyping;
    std::vector<uint8_t> canvas_;
    DirtyRectTracker dirty_tracker_;
};

uint64_t HashBytesFnv1a64(const uint8_t* data, size_t len);

} // namespace uu::core
