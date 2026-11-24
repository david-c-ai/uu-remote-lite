#include "capture_hal.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_OSX
#include <ApplicationServices/ApplicationServices.h>
#include <CoreGraphics/CoreGraphics.h>
#if defined(__OBJC__)
#import <Foundation/Foundation.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <dispatch/dispatch.h>
#endif
#endif
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wingdi.h>
#endif

namespace uu::core {

namespace {
uint64_t NowUs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}
} // namespace

uint64_t HashBytesFnv1a64(const uint8_t* data, size_t len) {
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < len; ++i) {
        h ^= static_cast<uint64_t>(data[i]);
        h *= 1099511628211ULL;
    }
    return h;
}

void DirtyRectTracker::Analyze(CapturedFrame& frame, bool force_full_frame) {
    frame.dirty_tiles.clear();
    const size_t expected_bytes = static_cast<size_t>(frame.width) * frame.height * 4;
    if (frame.bgra.size() < expected_bytes || frame.width == 0 || frame.height == 0) {
        frame.is_static_idle = true;
        frame.dirty_ratio = 0.0f;
        return;
    }

    const uint16_t tiles_x = (frame.width + kTileSize - 1) / kTileSize;
    const uint16_t tiles_y = (frame.height + kTileSize - 1) / kTileSize;
    const size_t total_tiles = static_cast<size_t>(tiles_x) * tiles_y;

    if (force_full_frame || prev_w_ != frame.width || prev_h_ != frame.height ||
        prev_bgra_.size() != expected_bytes) {
        prev_w_ = frame.width;
        prev_h_ = frame.height;
        prev_bgra_ = frame.bgra;
        for (uint16_t ty = 0; ty < tiles_y; ++ty) {
            for (uint16_t tx = 0; tx < tiles_x; ++tx) {
                uint16_t px = tx * kTileSize;
                uint16_t py = ty * kTileSize;
                uint16_t pw = std::min<uint16_t>(kTileSize, frame.width - px);
                uint16_t ph = std::min<uint16_t>(kTileSize, frame.height - py);
                frame.dirty_tiles.push_back({px, py, pw, ph});
            }
        }
        frame.is_static_idle = false;
        frame.dirty_ratio = 1.0f;
        return;
    }

    // Fast 64-bit word scan per 64x64 tile
    const uint32_t row_bytes = static_cast<uint32_t>(frame.width) * 4;
    for (uint16_t ty = 0; ty < tiles_y; ++ty) {
        uint16_t py = ty * kTileSize;
        uint16_t ph = std::min<uint16_t>(kTileSize, frame.height - py);

        for (uint16_t tx = 0; tx < tiles_x; ++tx) {
            uint16_t px = tx * kTileSize;
            uint16_t pw = std::min<uint16_t>(kTileSize, frame.width - px);
            const size_t tile_row_bytes = static_cast<size_t>(pw) * 4;

            bool changed = false;
            for (uint16_t r = 0; r < ph; ++r) {
                const size_t offset = static_cast<size_t>(py + r) * row_bytes + static_cast<size_t>(px) * 4;
                if (std::memcmp(frame.bgra.data() + offset, prev_bgra_.data() + offset, tile_row_bytes) != 0) {
                    changed = true;
                    break;
                }
            }

            if (changed) {
                for (uint16_t r = 0; r < ph; ++r) {
                    const size_t offset = static_cast<size_t>(py + r) * row_bytes + static_cast<size_t>(px) * 4;
                    std::memcpy(prev_bgra_.data() + offset, frame.bgra.data() + offset, tile_row_bytes);
                }
                frame.dirty_tiles.push_back({px, py, pw, ph});
            }
        }
    }

    frame.is_static_idle = frame.dirty_tiles.empty();
    frame.dirty_ratio = total_tiles > 0
                            ? static_cast<float>(frame.dirty_tiles.size()) / static_cast<float>(total_tiles)
                            : 0.0f;
}

// --- Synthetic Desktop Workload Capturer ---

SyntheticCapturer::SyntheticCapturer(uint16_t width, uint16_t height)
    : width_(width), height_(height), canvas_(static_cast<size_t>(width) * height * 4, 0) {
    // Initialize crisp dark IDE-style desktop background
    for (uint16_t y = 0; y < height_; ++y) {
        for (uint16_t x = 0; x < width_; ++x) {
            size_t idx = (static_cast<size_t>(y) * width_ + x) * 4;
            canvas_[idx + 0] = 36; // B
            canvas_[idx + 1] = 30; // G
            canvas_[idx + 2] = 26; // R
            canvas_[idx + 3] = 255;
            if ((x % 64 == 0) || (y == 32)) {
                canvas_[idx + 0] = 58;
                canvas_[idx + 1] = 52;
                canvas_[idx + 2] = 48;
            }
        }
    }
}

std::vector<MonitorInfo> SyntheticCapturer::EnumerateMonitors() {
    return {
        {0, 1, width_, height_, 144, true, "Primary Virtual Display"},
        {1, 2, 1920, 1080, 60, false, "Secondary Studio Display"}
    };
}

bool SyntheticCapturer::SelectMonitor(uint32_t) {
    dirty_tracker_.Reset();
    return true;
}

bool SyntheticCapturer::CaptureNext(CapturedFrame& out_frame, bool force_keyframe) {
    ++seq_;
    if (workload_ == SyntheticWorkload::kCodeEditorTyping) {
        // Simulate typing characters inside an active code line (only 1~2 tiles change!)
        uint16_t caret_x = static_cast<uint16_t>(128 + ((seq_ * 12) % (width_ / 2)));
        uint16_t caret_y = static_cast<uint16_t>(160 + (((seq_ / 20) * 28) % (height_ / 2)));
        for (uint16_t dy = 0; dy < 18 && (caret_y + dy) < height_; ++dy) {
            for (uint16_t dx = 0; dx < 10 && (caret_x + dx) < width_; ++dx) {
                size_t idx = (static_cast<size_t>(caret_y + dy) * width_ + (caret_x + dx)) * 4;
                canvas_[idx + 0] = static_cast<uint8_t>(180 + (seq_ * 17) % 70);
                canvas_[idx + 1] = 220;
                canvas_[idx + 2] = 140;
                canvas_[idx + 3] = 255;
            }
        }
    } else if (workload_ == SyntheticWorkload::kFullMotionGaming) {
        // Realistic smooth 60/144fps moving camera viewport & HUD bars
        int shift_x = static_cast<int>(seq_ * 8);
        int shift_y = static_cast<int>(seq_ * 4);
        for (uint16_t y = 0; y < height_; ++y) {
            uint8_t sky_b = static_cast<uint8_t>(60 + (y * 120) / height_);
            uint8_t sky_g = static_cast<uint8_t>(35 + (y * 80) / height_);
            size_t row_off = static_cast<size_t>(y) * width_ * 4;
            for (uint16_t x = 0; x < width_; ++x) {
                size_t idx = row_off + static_cast<size_t>(x) * 4;
                bool checker = (((x + shift_x) / 48) + ((y + shift_y) / 48)) % 2 == 0;
                canvas_[idx + 0] = checker ? sky_b : static_cast<uint8_t>(sky_b / 2);
                canvas_[idx + 1] = checker ? sky_g : static_cast<uint8_t>(sky_g + 25);
                canvas_[idx + 2] = checker ? 42 : 88;
                canvas_[idx + 3] = 255;
            }
        }
    }

    out_frame.frame_id = seq_;
    out_frame.width = width_;
    out_frame.height = height_;
    out_frame.stride_bytes = static_cast<uint32_t>(width_) * 4;
    out_frame.capture_ts_us = NowUs();
    out_frame.bgra = canvas_;
    dirty_tracker_.Analyze(out_frame, force_keyframe || seq_ == 1);
    return true;
}

CursorSnapshot SyntheticCapturer::CaptureCursor() {
    CursorSnapshot cur{};
    double angle = static_cast<double>(seq_) * 0.08;
    cur.x_norm = static_cast<uint16_t>(32768.0 + 18000.0 * std::cos(angle));
    cur.y_norm = static_cast<uint16_t>(32768.0 + 18000.0 * std::sin(angle));
    cur.width = 16;
    cur.height = 16;
    cur.hotspot_x = 1;
    cur.hotspot_y = 1;

    std::vector<uint8_t> arrow(16 * 16 * 4, 0);
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x <= y && x < 12; ++x) {
            size_t off = static_cast<size_t>(y * 16 + x) * 4;
            arrow[off + 0] = 255;
            arrow[off + 1] = 255;
            arrow[off + 2] = 255;
            arrow[off + 3] = 255;
        }
    }
    uint64_t h = HashBytesFnv1a64(arrow.data(), arrow.size());
    cur.shape_hash = h;
    if (h != last_cursor_hash_) {
        last_cursor_hash_ = h;
        cur.shape_changed = true;
        cur.bgra_pixels = std::move(arrow);
    }
    return cur;
}

// --- OS-Native Display Capturer ---

#if defined(__APPLE__) && TARGET_OS_OSX
class MacNativeCapturer : public IScreenCapturer {
public:
    MacNativeCapturer() {
        auto mons = EnumerateMonitors();
        if (!mons.empty()) {
            active_display_ = mons[0].os_display_id;
            active_w_ = mons[0].width;
            active_h_ = mons[0].height;
        } else {
            active_display_ = CGMainDisplayID();
        }
    }

    std::vector<MonitorInfo> EnumerateMonitors() override {
        std::vector<MonitorInfo> list;
        CGDirectDisplayID ids[16];
        uint32_t count = 0;
        if (CGGetActiveDisplayList(16, ids, &count) == kCGErrorSuccess) {
            for (uint32_t i = 0; i < count; ++i) {
                MonitorInfo m{};
                m.index = i;
                m.os_display_id = ids[i];
                m.width = static_cast<uint16_t>(CGDisplayPixelsWide(ids[i]));
                m.height = static_cast<uint16_t>(CGDisplayPixelsHigh(ids[i]));
                m.is_primary = CGDisplayIsMain(ids[i]) != 0;
                CGDisplayModeRef mode = CGDisplayCopyDisplayMode(ids[i]);
                if (mode) {
                    double hz = CGDisplayModeGetRefreshRate(mode);
                    m.refresh_hz = hz > 1.0 ? static_cast<uint16_t>(hz) : 60;
                    CFRelease(mode);
                }
                m.name = m.is_primary ? "macOS Main Display" : ("macOS Display #" + std::to_string(i + 1));
                list.push_back(m);
            }
        }
        return list;
    }

    bool SelectMonitor(uint32_t monitor_index) override {
        auto mons = EnumerateMonitors();
        if (monitor_index >= mons.size()) return false;
        active_display_ = mons[monitor_index].os_display_id;
        active_w_ = mons[monitor_index].width;
        active_h_ = mons[monitor_index].height;
        dirty_tracker_.Reset();
        return true;
    }

    bool CaptureNext(CapturedFrame& out_frame, bool force_keyframe) override {
        CGImageRef img = nullptr;
#if defined(__OBJC__)
        if (@available(macOS 14.0, *)) {
            if (CGPreflightScreenCaptureAccess()) {
                __block CGImageRef captured = nullptr;
                dispatch_semaphore_t sem = dispatch_semaphore_create(0);
                [SCShareableContent getShareableContentWithCompletionHandler:^(
                                        SCShareableContent* content, NSError* err) {
                    if (!content || err || content.displays.count == 0) {
                        dispatch_semaphore_signal(sem);
                        return;
                    }
                    SCDisplay* target = content.displays.firstObject;
                    for (SCDisplay* d in content.displays) {
                        if (d.displayID == active_display_) {
                            target = d;
                            break;
                        }
                    }
                    SCContentFilter* filter = [[SCContentFilter alloc] initWithDisplay:target
                                                                      excludingWindows:@[]];
                    SCStreamConfiguration* cfg = [[SCStreamConfiguration alloc] init];
                    cfg.width = target.width;
                    cfg.height = target.height;
                    cfg.showsCursor = NO; // Decoupled cursor channel!
                    [SCScreenshotManager captureImageWithFilter:filter
                                                  configuration:cfg
                                              completionHandler:^(CGImageRef sample, NSError* sErr) {
                                                  if (sample && !sErr) {
                                                      captured = CGImageRetain(sample);
                                                  }
                                                  dispatch_semaphore_signal(sem);
                                              }];
                }];
                dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 120 * NSEC_PER_MSEC));
                img = captured;
            }
        }
#endif
        if (!img) {
            return fallback_.CaptureNext(out_frame, force_keyframe);
        }
        size_t w = CGImageGetWidth(img);
        size_t h = CGImageGetHeight(img);
        if (w == 0 || h == 0) {
            CFRelease(img);
            return fallback_.CaptureNext(out_frame, force_keyframe);
        }

        out_frame.frame_id = ++seq_;
        out_frame.width = static_cast<uint16_t>(w);
        out_frame.height = static_cast<uint16_t>(h);
        out_frame.stride_bytes = static_cast<uint32_t>(w * 4);
        out_frame.capture_ts_us = NowUs();
        out_frame.bgra.resize(w * h * 4);

        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        CGBitmapInfo bmp_info = static_cast<CGBitmapInfo>(kCGBitmapByteOrder32Little) |
                                static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedFirst);
        CGContextRef ctx = CGBitmapContextCreate(
            out_frame.bgra.data(), w, h, 8, w * 4, cs, bmp_info);
        if (ctx) {
            CGContextDrawImage(ctx, CGRectMake(0, 0, static_cast<CGFloat>(w), static_cast<CGFloat>(h)), img);
            CGContextRelease(ctx);
        }
        CGColorSpaceRelease(cs);
        CFRelease(img);

        dirty_tracker_.Analyze(out_frame, force_keyframe || seq_ == 1);
        return true;
    }

    CursorSnapshot CaptureCursor() override {
        CursorSnapshot cur = fallback_.CaptureCursor();
        CGEventRef ev = CGEventCreate(nullptr);
        if (ev) {
            CGPoint pt = CGEventGetLocation(ev);
            CFRelease(ev);
            uint16_t w = std::max<uint16_t>(1, active_w_);
            uint16_t h = std::max<uint16_t>(1, active_h_);
            double nx = std::clamp(pt.x / static_cast<double>(w), 0.0, 1.0);
            double ny = std::clamp(pt.y / static_cast<double>(h), 0.0, 1.0);
            cur.x_norm = static_cast<uint16_t>(nx * 65535.0);
            cur.y_norm = static_cast<uint16_t>(ny * 65535.0);
        }
        return cur;
    }

private:
    CGDirectDisplayID active_display_ = 0;
    uint16_t active_w_ = 1920;
    uint16_t active_h_ = 1080;
    uint32_t seq_ = 0;
    DirtyRectTracker dirty_tracker_;
    SyntheticCapturer fallback_{1280, 720};
};
#endif

#if defined(_WIN32)
class WinNativeCapturer : public IScreenCapturer {
public:
    WinNativeCapturer() {
        SetProcessDPIAware();
        RefreshMetrics();
    }

    ~WinNativeCapturer() override {
        ReleaseDib();
    }

    std::vector<MonitorInfo> EnumerateMonitors() override {
        std::vector<MonitorInfo> mons;
        EnumDisplayMonitors(
            nullptr, nullptr,
            [](HMONITOR hmon, HDC, LPRECT, LPARAM lparam) -> BOOL {
                auto* out = reinterpret_cast<std::vector<MonitorInfo>*>(lparam);
                MONITORINFOEXA mi{};
                mi.cbSize = sizeof(mi);
                if (GetMonitorInfoA(hmon, &mi)) {
                    MonitorInfo info{};
                    info.index = static_cast<uint32_t>(out->size());
                    info.os_display_id = info.index + 1;
                    info.width = static_cast<uint16_t>(mi.rcMonitor.right - mi.rcMonitor.left);
                    info.height = static_cast<uint16_t>(mi.rcMonitor.bottom - mi.rcMonitor.top);
                    info.is_primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
                    info.refresh_hz = 60;
                    DEVMODEA dm{};
                    dm.dmSize = sizeof(dm);
                    if (EnumDisplaySettingsA(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) &&
                        dm.dmDisplayFrequency > 1) {
                        info.refresh_hz = static_cast<uint16_t>(dm.dmDisplayFrequency);
                    }
                    info.name = mi.szDevice;
                    out->push_back(info);
                }
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&mons));

        if (mons.empty()) {
            MonitorInfo m{};
            m.width = static_cast<uint16_t>(GetSystemMetrics(SM_CXSCREEN));
            m.height = static_cast<uint16_t>(GetSystemMetrics(SM_CYSCREEN));
            m.name = "Windows Primary Display";
            mons.push_back(m);
        }
        return mons;
    }

    bool SelectMonitor(uint32_t monitor_index) override {
        std::vector<RECT> rects;
        EnumDisplayMonitors(
            nullptr, nullptr,
            [](HMONITOR hmon, HDC, LPRECT, LPARAM lparam) -> BOOL {
                auto* vec = reinterpret_cast<std::vector<RECT>*>(lparam);
                MONITORINFO mi{};
                mi.cbSize = sizeof(mi);
                if (GetMonitorInfo(hmon, &mi)) {
                    vec->push_back(mi.rcMonitor);
                }
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&rects));

        if (monitor_index < rects.size()) {
            origin_x_ = rects[monitor_index].left;
            origin_y_ = rects[monitor_index].top;
            active_w_ = static_cast<uint16_t>(rects[monitor_index].right - rects[monitor_index].left);
            active_h_ = static_cast<uint16_t>(rects[monitor_index].bottom - rects[monitor_index].top);
            ReleaseDib();
            dirty_tracker_.Reset();
            return true;
        }
        return false;
    }

    bool CaptureNext(CapturedFrame& out_frame, bool force_keyframe) override {
        if (active_w_ == 0 || active_h_ == 0) RefreshMetrics();
        if (active_w_ == 0 || active_h_ == 0) {
            return fallback_.CaptureNext(out_frame, force_keyframe);
        }

        HDC screen_dc = GetDC(nullptr);
        if (!screen_dc) return fallback_.CaptureNext(out_frame, force_keyframe);

        if (!mem_dc_ || !dib_ || dib_w_ != active_w_ || dib_h_ != active_h_) {
            ReleaseDib();
            mem_dc_ = CreateCompatibleDC(screen_dc);
            BITMAPINFO bmi{};
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = active_w_;
            bmi.bmiHeader.biHeight = -static_cast<LONG>(active_h_); // Top-down BGRA
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;
            dib_ = CreateDIBSection(screen_dc, &bmi, DIB_RGB_COLORS, &dib_bits_, nullptr, 0);
            if (mem_dc_ && dib_) {
                old_obj_ = SelectObject(mem_dc_, dib_);
                dib_w_ = active_w_;
                dib_h_ = active_h_;
            }
        }

        bool ok = false;
        if (mem_dc_ && dib_ && dib_bits_) {
            if (BitBlt(mem_dc_, 0, 0, active_w_, active_h_, screen_dc, origin_x_, origin_y_, SRCCOPY)) {
                size_t total = static_cast<size_t>(active_w_) * active_h_ * 4;
                out_frame.frame_id = ++seq_;
                out_frame.width = active_w_;
                out_frame.height = active_h_;
                out_frame.stride_bytes = static_cast<uint32_t>(active_w_) * 4;
                out_frame.capture_ts_us = NowUs();
                out_frame.bgra.resize(total);
                std::memcpy(out_frame.bgra.data(), dib_bits_, total);
                ok = true;
            }
        }
        ReleaseDC(nullptr, screen_dc);

        if (!ok) return fallback_.CaptureNext(out_frame, force_keyframe);
        dirty_tracker_.Analyze(out_frame, force_keyframe || seq_ == 1);
        return true;
    }

    CursorSnapshot CaptureCursor() override {
        CursorSnapshot cur = fallback_.CaptureCursor();
        POINT pt{};
        if (GetCursorPos(&pt) && active_w_ > 0 && active_h_ > 0) {
            double nx = std::clamp(static_cast<double>(pt.x - origin_x_) / active_w_, 0.0, 1.0);
            double ny = std::clamp(static_cast<double>(pt.y - origin_y_) / active_h_, 0.0, 1.0);
            cur.x_norm = static_cast<uint16_t>(nx * 65535.0);
            cur.y_norm = static_cast<uint16_t>(ny * 65535.0);
        }
        return cur;
    }

private:
    void RefreshMetrics() {
        origin_x_ = 0;
        origin_y_ = 0;
        active_w_ = static_cast<uint16_t>(GetSystemMetrics(SM_CXSCREEN));
        active_h_ = static_cast<uint16_t>(GetSystemMetrics(SM_CYSCREEN));
    }

    void ReleaseDib() {
        if (mem_dc_ && old_obj_) {
            SelectObject(mem_dc_, old_obj_);
            old_obj_ = nullptr;
        }
        if (dib_) {
            DeleteObject(dib_);
            dib_ = nullptr;
        }
        if (mem_dc_) {
            DeleteDC(mem_dc_);
            mem_dc_ = nullptr;
        }
        dib_bits_ = nullptr;
        dib_w_ = 0;
        dib_h_ = 0;
    }

    int origin_x_ = 0;
    int origin_y_ = 0;
    uint16_t active_w_ = 0;
    uint16_t active_h_ = 0;
    HDC mem_dc_ = nullptr;
    HBITMAP dib_ = nullptr;
    HGDIOBJ old_obj_ = nullptr;
    void* dib_bits_ = nullptr;
    uint16_t dib_w_ = 0;
    uint16_t dib_h_ = 0;
    uint32_t seq_ = 0;
    DirtyRectTracker dirty_tracker_;
    SyntheticCapturer fallback_{1280, 720};
};
#endif

std::unique_ptr<IScreenCapturer> CreateNativeCapturer() {
#if defined(__APPLE__) && TARGET_OS_OSX
    return std::make_unique<MacNativeCapturer>();
#elif defined(_WIN32)
    return std::make_unique<WinNativeCapturer>();
#else
    return std::make_unique<SyntheticCapturer>(1280, 720);
#endif
}

} // namespace uu::core
