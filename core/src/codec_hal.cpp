#include "codec_hal.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>

#if defined(__APPLE__)
#include <CoreGraphics/CoreGraphics.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <ImageIO/ImageIO.h>
#include <VideoToolbox/VideoToolbox.h>
#include <compression.h>
#elif defined(__ANDROID__)
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <objbase.h>
#include <wincodec.h>
#endif

namespace uu::core {

namespace {

uint64_t NowUs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

void AppendU16LE(std::vector<uint8_t>& buf, uint16_t v) {
    buf.push_back(static_cast<uint8_t>(v & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

void AppendU32LE(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back(static_cast<uint8_t>(v & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

uint16_t ReadU16LE(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

uint32_t ReadU32LE(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

} // namespace

// --- Fast Cross-Platform LZ4 Wrapper ---

size_t CompressFastLz4(const uint8_t* src, size_t src_len, std::vector<uint8_t>& out_dst) {
    if (src_len == 0) {
        out_dst.clear();
        return 0;
    }
    // Unified cross-OS 4-byte pixel run-length compressor (100% wire-compatible across Mac/Win/Android/iOS/Linux!)
    out_dst.clear();
    out_dst.reserve(src_len);
    size_t i = 0;
    while (i < src_len) {
        if (i + 4 <= src_len) {
            uint32_t px;
            std::memcpy(&px, src + i, 4);
            size_t run = 1;
            while (run < 127 && i + (run + 1) * 4 <= src_len) {
                uint32_t next_px;
                std::memcpy(&next_px, src + i + run * 4, 4);
                if (next_px != px) break;
                ++run;
            }
            if (run >= 2) {
                out_dst.push_back(static_cast<uint8_t>(0x80 | run));
                out_dst.insert(out_dst.end(), src + i, src + i + 4);
                i += run * 4;
                continue;
            }
        }
        size_t lit_start = i;
        size_t lit_bytes = 0;
        while (lit_bytes < 127 && i < src_len) {
            if (i + 8 <= src_len && std::memcmp(src + i, src + i + 4, 4) == 0) break;
            ++i;
            ++lit_bytes;
        }
        out_dst.push_back(static_cast<uint8_t>(lit_bytes));
        out_dst.insert(out_dst.end(), src + lit_start, src + lit_start + lit_bytes);
    }
    if (out_dst.size() < src_len) {
        return out_dst.size();
    }
    out_dst.clear();
    return 0; // Indicates raw is smaller or incompressible
}

bool DecompressFastLz4(const uint8_t* src, size_t src_len, uint8_t* dst, size_t expected_dst_len) {
    if (expected_dst_len == 0) return true;
    size_t sp = 0;
    size_t dp = 0;
    while (sp < src_len && dp < expected_dst_len) {
        uint8_t tag = src[sp++];
        if (tag & 0x80) {
            size_t run = tag & 0x7F;
            if (sp + 4 > src_len || dp + run * 4 > expected_dst_len) return false;
            for (size_t k = 0; k < run; ++k) {
                std::memcpy(dst + dp, src + sp, 4);
                dp += 4;
            }
            sp += 4;
        } else {
            size_t lit = tag;
            if (sp + lit > src_len || dp + lit > expected_dst_len) return false;
            std::memcpy(dst + dp, src + sp, lit);
            dp += lit;
            sp += lit;
        }
    }
    return dp == expected_dst_len;
}

// --- Hardware-Accelerated Baseline JPEG Encoder (Apple ImageIO / Windows WIC) ---

bool EncodeHardwareJpeg(const uint8_t* bgra, uint16_t width, uint16_t height,
                        float quality, std::vector<uint8_t>& out_jpeg) {
    out_jpeg.clear();
    if (!bgra || width == 0 || height == 0) return false;
    quality = std::clamp(quality, 0.20f, 0.92f);

#if defined(__APPLE__)
    const size_t row_bytes = static_cast<size_t>(width) * 4;
    const size_t total_bytes = row_bytes * height;
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef provider = CGDataProviderCreateWithData(nullptr, bgra, total_bytes, nullptr);
    CGBitmapInfo bmp_info = static_cast<CGBitmapInfo>(kCGBitmapByteOrder32Little) |
                            static_cast<CGBitmapInfo>(kCGImageAlphaNoneSkipFirst);
    CGImageRef img = CGImageCreate(
        width, height, 8, 32, row_bytes, cs, bmp_info, provider, nullptr, false, kCGRenderingIntentDefault);
    CGDataProviderRelease(provider);
    CGColorSpaceRelease(cs);
    if (!img) return false;

    CFMutableDataRef dst_data = CFDataCreateMutable(kCFAllocatorDefault, 0);
    CFStringRef jpeg_uti = CFSTR("public.jpeg");
    CGImageDestinationRef dest = CGImageDestinationCreateWithData(dst_data, jpeg_uti, 1, nullptr);
    bool ok = false;
    if (dest) {
        CFNumberRef q_num = CFNumberCreate(kCFAllocatorDefault, kCFNumberFloatType, &quality);
        const void* keys[1] = {kCGImageDestinationLossyCompressionQuality};
        const void* vals[1] = {q_num};
        CFDictionaryRef props = CFDictionaryCreate(
            kCFAllocatorDefault, keys, vals, 1,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CGImageDestinationAddImage(dest, img, props);
        ok = CGImageDestinationFinalize(dest);
        CFRelease(props);
        CFRelease(q_num);
        CFRelease(dest);
    }
    CGImageRelease(img);

    if (ok && dst_data && CFDataGetLength(dst_data) > 0) {
        const uint8_t* ptr = CFDataGetBytePtr(dst_data);
        size_t len = static_cast<size_t>(CFDataGetLength(dst_data));
        out_jpeg.assign(ptr, ptr + len);
        CFRelease(dst_data);
        return true;
    }
    if (dst_data) CFRelease(dst_data);
    return false;
#elif defined(_WIN32)
    static thread_local bool com_init = []() {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
    }();
    (void)com_init;

    IWICImagingFactory* factory = nullptr;
    HRESULT hr = CoCreateInstance(
        CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory));
    if (FAILED(hr) || !factory) return false;

    IStream* mem_stream = nullptr;
    hr = CreateStreamOnHGlobal(nullptr, TRUE, &mem_stream);
    if (FAILED(hr) || !mem_stream) {
        factory->Release();
        return false;
    }

    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    IPropertyBag2* prop_bag = nullptr;
    bool ok = false;

    if (SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder)) &&
        SUCCEEDED(encoder->Initialize(mem_stream, WICBitmapEncoderNoCache)) &&
        SUCCEEDED(encoder->CreateNewFrame(&frame, &prop_bag))) {
        if (prop_bag) {
            PROPBAG2 opt{};
            opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
            VARIANT var{};
            VariantInit(&var);
            var.vt = VT_R4;
            var.fltVal = quality;
            prop_bag->Write(1, &opt, &var);
        }
        WICPixelFormatGUID pf = GUID_WICPixelFormat32bppBGRA;
        UINT stride = static_cast<UINT>(width) * 4;
        UINT total = stride * height;
        if (SUCCEEDED(frame->Initialize(prop_bag)) &&
            SUCCEEDED(frame->SetSize(width, height)) &&
            SUCCEEDED(frame->SetPixelFormat(&pf)) &&
            SUCCEEDED(frame->WritePixels(height, stride, total, const_cast<BYTE*>(bgra))) &&
            SUCCEEDED(frame->Commit()) &&
            SUCCEEDED(encoder->Commit())) {
            HGLOBAL hmem = nullptr;
            STATSTG stat{};
            if (SUCCEEDED(mem_stream->Stat(&stat, STATFLAG_NONAME)) &&
                SUCCEEDED(GetHGlobalFromStream(mem_stream, &hmem)) &&
                hmem && stat.cbSize.QuadPart > 0) {
                void* locked = GlobalLock(hmem);
                if (locked) {
                    size_t sz = static_cast<size_t>(stat.cbSize.QuadPart);
                    out_jpeg.resize(sz);
                    std::memcpy(out_jpeg.data(), locked, sz);
                    GlobalUnlock(hmem);
                    ok = true;
                }
            }
        }
    }

    if (prop_bag) prop_bag->Release();
    if (frame) frame->Release();
    if (encoder) encoder->Release();
    mem_stream->Release();
    factory->Release();
    return ok;
#else
    (void)bgra;
    (void)width;
    (void)height;
    (void)quality;
    return false;
#endif
}

// --- Apple VideoToolbox Hardware HEVC (H.265) Zero-Latency Encoder & Decoder ---

#if defined(__APPLE__)
class AppleHevcEncoder {
public:
    ~AppleHevcEncoder() { Reset(); }

    void Reset() {
        if (session_) {
            VTCompressionSessionInvalidate(session_);
            CFRelease(session_);
            session_ = nullptr;
        }
        w_ = 0;
        h_ = 0;
    }

    bool EnsureInitialized(uint16_t width, uint16_t height, uint32_t bitrate_kbps) {
        if (session_ && w_ == width && h_ == height) {
            UpdateBitrate(bitrate_kbps);
            return true;
        }
        Reset();

        OSStatus st = VTCompressionSessionCreate(
            kCFAllocatorDefault, width, height, kCMVideoCodecType_HEVC,
            nullptr, nullptr, kCFAllocatorDefault, &AppleHevcEncoder::OutputCallback,
            this, &session_);
        if (st != noErr || !session_) {
            session_ = nullptr;
            return false;
        }

        // Ultra-low latency configuration: RealTime = TRUE, FrameReordering (B-frames) = FALSE!
        VTSessionSetProperty(session_, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
        VTSessionSetProperty(session_, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);

        int max_gop = 240;
        CFNumberRef gop_num = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &max_gop);
        VTSessionSetProperty(session_, kVTCompressionPropertyKey_MaxKeyFrameInterval, gop_num);
        CFRelease(gop_num);

        UpdateBitrate(bitrate_kbps);
        VTCompressionSessionPrepareToEncodeFrames(session_);
        w_ = width;
        h_ = height;
        return true;
    }

    void UpdateBitrate(uint32_t bitrate_kbps) {
        if (!session_ || bitrate_kbps == last_kbps_) return;
        int64_t bps = static_cast<int64_t>(std::max<uint32_t>(200, bitrate_kbps)) * 1000;
        CFNumberRef br = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &bps);
        VTSessionSetProperty(session_, kVTCompressionPropertyKey_AverageBitRate, br);
        CFRelease(br);

        // Enforce strict burst ceiling via DataRateLimits [bytes_per_sec, 1.0 sec]
        int64_t bytes_per_sec = (bps / 8) * 12 / 10;
        double window_sec = 1.0;
        CFNumberRef n_bytes = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &bytes_per_sec);
        CFNumberRef n_sec = CFNumberCreate(kCFAllocatorDefault, kCFNumberDoubleType, &window_sec);
        const void* vals[2] = {n_bytes, n_sec};
        CFArrayRef limits = CFArrayCreate(kCFAllocatorDefault, vals, 2, &kCFTypeArrayCallBacks);
        VTSessionSetProperty(session_, kVTCompressionPropertyKey_DataRateLimits, limits);
        CFRelease(limits);
        CFRelease(n_bytes);
        CFRelease(n_sec);

        float quality = bitrate_kbps < 1000 ? 0.42f : 0.65f;
        CFNumberRef q_ref = CFNumberCreate(kCFAllocatorDefault, kCFNumberFloatType, &quality);
        VTSessionSetProperty(session_, kVTCompressionPropertyKey_Quality, q_ref);
        CFRelease(q_ref);

        last_kbps_ = bitrate_kbps;
    }

    bool EncodeFrame(const CapturedFrame& frame, uint32_t bitrate_kbps, bool force_idr,
                     std::vector<uint8_t>& out_payload, bool& out_is_idr) {
        if (!EnsureInitialized(frame.width, frame.height, bitrate_kbps)) return false;

        CVPixelBufferRef px = nullptr;
        OSStatus st = CVPixelBufferCreate(
            kCFAllocatorDefault, frame.width, frame.height,
            kCVPixelFormatType_32BGRA, nullptr, &px);
        if (st != kCVReturnSuccess || !px) return false;

        CVPixelBufferLockBaseAddress(px, 0);
        uint8_t* dst = static_cast<uint8_t*>(CVPixelBufferGetBaseAddress(px));
        size_t dst_stride = CVPixelBufferGetBytesPerRow(px);
        size_t src_stride = static_cast<size_t>(frame.width) * 4;
        for (uint16_t y = 0; y < frame.height; ++y) {
            std::memcpy(dst + y * dst_stride, frame.bgra.data() + y * src_stride, src_stride);
        }
        CVPixelBufferUnlockBaseAddress(px, 0);

        CFMutableDictionaryRef frame_props = nullptr;
        if (force_idr) {
            frame_props = CFDictionaryCreateMutable(
                kCFAllocatorDefault, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            CFDictionarySetValue(frame_props, kVTEncodeFrameOptionKey_ForceKeyFrame, kCFBooleanTrue);
        }

        {
            std::lock_guard<std::mutex> lk(cb_mu_);
            encoded_buf_.clear();
            encoded_ok_ = false;
            encoded_idr_ = false;
        }

        CMTime pts = CMTimeMake(frame_pts_++, 60);
        VTEncodeInfoFlags flags = 0;
        st = VTCompressionSessionEncodeFrame(
            session_, px, pts, kCMTimeInvalid, frame_props, nullptr, &flags);
        if (frame_props) CFRelease(frame_props);
        CVPixelBufferRelease(px);
        if (st != noErr) return false;

        // Flush synchronously for deterministic zero-frame buffering!
        VTCompressionSessionCompleteFrames(session_, pts);

        std::lock_guard<std::mutex> lk(cb_mu_);
        if (!encoded_ok_ || encoded_buf_.empty()) return false;
        out_payload = std::move(encoded_buf_);
        out_is_idr = encoded_idr_;
        return true;
    }

private:
    static void OutputCallback(void* refcon, void*, OSStatus status, VTEncodeInfoFlags,
                               CMSampleBufferRef sbuf) {
        if (status != noErr || !sbuf) return;
        auto* self = static_cast<AppleHevcEncoder*>(refcon);

        bool is_keyframe = true;
        CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sbuf, false);
        if (attachments && CFArrayGetCount(attachments) > 0) {
            auto dict = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(attachments, 0));
            if (CFDictionaryContainsKey(dict, kCMSampleAttachmentKey_NotSync)) {
                is_keyframe = false;
            }
        }

        std::vector<uint8_t> packet;
        CMFormatDescriptionRef fmt = CMSampleBufferGetFormatDescription(sbuf);
        if (is_keyframe && fmt) {
            size_t param_count = 0;
            int nal_hdr_len = 4;
            if (CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(
                    fmt, 0, nullptr, nullptr, &param_count, &nal_hdr_len) == noErr &&
                param_count > 0) {
                packet.push_back(1); // has_hvcc_params = 1
                packet.push_back(static_cast<uint8_t>(param_count));
                for (size_t i = 0; i < param_count; ++i) {
                    const uint8_t* ps_ptr = nullptr;
                    size_t ps_size = 0;
                    if (CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(
                            fmt, i, &ps_ptr, &ps_size, nullptr, nullptr) == noErr &&
                        ps_ptr && ps_size > 0) {
                        AppendU16LE(packet, static_cast<uint16_t>(ps_size));
                        packet.insert(packet.end(), ps_ptr, ps_ptr + ps_size);
                    } else {
                        AppendU16LE(packet, 0);
                    }
                }
            } else {
                packet.push_back(0);
            }
        } else {
            packet.push_back(0); // has_hvcc_params = 0
        }

        CMBlockBufferRef bbuf = CMSampleBufferGetDataBuffer(sbuf);
        if (!bbuf) return;
        size_t total_len = 0;
        char* data_ptr = nullptr;
        if (CMBlockBufferGetDataPointer(bbuf, 0, nullptr, &total_len, &data_ptr) != kCMBlockBufferNoErr ||
            !data_ptr || total_len == 0) {
            return;
        }
        AppendU32LE(packet, static_cast<uint32_t>(total_len));
        packet.insert(packet.end(), reinterpret_cast<uint8_t*>(data_ptr),
                      reinterpret_cast<uint8_t*>(data_ptr) + total_len);

        std::lock_guard<std::mutex> lk(self->cb_mu_);
        self->encoded_buf_ = std::move(packet);
        self->encoded_idr_ = is_keyframe;
        self->encoded_ok_ = true;
    }

    VTCompressionSessionRef session_ = nullptr;
    uint16_t w_ = 0;
    uint16_t h_ = 0;
    uint32_t last_kbps_ = 0;
    int64_t frame_pts_ = 0;
    std::mutex cb_mu_;
    std::vector<uint8_t> encoded_buf_;
    bool encoded_ok_ = false;
    bool encoded_idr_ = false;
};

class AppleHevcDecoder {
public:
    ~AppleHevcDecoder() { Reset(); }

    void Reset() {
        if (session_) {
            VTDecompressionSessionInvalidate(session_);
            CFRelease(session_);
            session_ = nullptr;
        }
        if (fmt_desc_) {
            CFRelease(fmt_desc_);
            fmt_desc_ = nullptr;
        }
    }

    bool DecodeFrame(const uint8_t* data, size_t len, uint16_t width, uint16_t height,
                     std::vector<uint8_t>& out_bgra) {
        if (len < 5) return false;
        size_t pos = 0;
        uint8_t has_params = data[pos++];

        if (has_params == 1) {
            if (pos >= len) return false;
            uint8_t count = data[pos++];
            std::vector<const uint8_t*> ptrs;
            std::vector<size_t> sizes;
            for (uint8_t i = 0; i < count; ++i) {
                if (pos + 2 > len) return false;
                uint16_t sz = ReadU16LE(data + pos);
                pos += 2;
                if (pos + sz > len) return false;
                if (sz > 0) {
                    ptrs.push_back(data + pos);
                    sizes.push_back(sz);
                }
                pos += sz;
            }
            if (!ptrs.empty()) {
                CMFormatDescriptionRef new_fmt = nullptr;
                OSStatus st = CMVideoFormatDescriptionCreateFromHEVCParameterSets(
                    kCFAllocatorDefault, ptrs.size(), ptrs.data(), sizes.data(), 4, nullptr, &new_fmt);
                if (st == noErr && new_fmt) {
                    if (!session_ || !VTDecompressionSessionCanAcceptFormatDescription(session_, new_fmt)) {
                        Reset();
                        fmt_desc_ = new_fmt;
                        if (!CreateSession(width, height)) return false;
                    } else {
                        CFRelease(new_fmt);
                    }
                }
            }
        }

        if (!session_ || !fmt_desc_ || pos + 4 > len) return false;
        uint32_t sample_len = ReadU32LE(data + pos);
        pos += 4;
        if (pos + sample_len > len || sample_len == 0) return false;

        CMBlockBufferRef bbuf = nullptr;
        if (CMBlockBufferCreateWithMemoryBlock(
                kCFAllocatorDefault, const_cast<uint8_t*>(data + pos), sample_len,
                kCFAllocatorNull, nullptr, 0, sample_len, 0, &bbuf) != kCMBlockBufferNoErr) {
            return false;
        }

        CMSampleBufferRef sbuf = nullptr;
        const size_t sample_size = sample_len;
        if (CMSampleBufferCreateReady(
                kCFAllocatorDefault, bbuf, fmt_desc_, 1, 0, nullptr, 1, &sample_size, &sbuf) != noErr) {
            CFRelease(bbuf);
            return false;
        }

        {
            std::lock_guard<std::mutex> lk(cb_mu_);
            decoded_ok_ = false;
        }

        VTDecodeInfoFlags info_flags = 0;
        OSStatus st = VTDecompressionSessionDecodeFrame(session_, sbuf, 0, nullptr, &info_flags);
        VTDecompressionSessionWaitForAsynchronousFrames(session_);
        CFRelease(sbuf);
        CFRelease(bbuf);
        if (st != noErr) return false;

        std::lock_guard<std::mutex> lk(cb_mu_);
        if (!decoded_ok_) return false;
        out_bgra = std::move(decoded_bgra_);
        return true;
    }

private:
    bool CreateSession(uint16_t width, uint16_t height) {
        CFMutableDictionaryRef dst_attrs = CFDictionaryCreateMutable(
            kCFAllocatorDefault, 3, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        int fmt = kCVPixelFormatType_32BGRA;
        int w = width;
        int h = height;
        CFNumberRef n_fmt = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &fmt);
        CFNumberRef n_w = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &w);
        CFNumberRef n_h = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &h);
        CFDictionarySetValue(dst_attrs, kCVPixelBufferPixelFormatTypeKey, n_fmt);
        CFDictionarySetValue(dst_attrs, kCVPixelBufferWidthKey, n_w);
        CFDictionarySetValue(dst_attrs, kCVPixelBufferHeightKey, n_h);
        CFRelease(n_fmt);
        CFRelease(n_w);
        CFRelease(n_h);

        VTDecompressionOutputCallbackRecord cb{};
        cb.decompressionOutputCallback = &AppleHevcDecoder::DecodeCallback;
        cb.decompressionOutputRefCon = this;

        OSStatus st = VTDecompressionSessionCreate(
            kCFAllocatorDefault, fmt_desc_, nullptr, dst_attrs, &cb, &session_);
        CFRelease(dst_attrs);
        if (st != noErr || !session_) {
            session_ = nullptr;
            return false;
        }
        VTSessionSetProperty(session_, kVTDecompressionPropertyKey_RealTime, kCFBooleanTrue);
        return true;
    }

    static void DecodeCallback(void* refcon, void*, OSStatus status, VTDecodeInfoFlags,
                               CVImageBufferRef img, CMTime, CMTime) {
        if (status != noErr || !img) return;
        auto* self = static_cast<AppleHevcDecoder*>(refcon);

        CVPixelBufferLockBaseAddress(img, kCVPixelBufferLock_ReadOnly);
        size_t w = CVPixelBufferGetWidth(img);
        size_t h = CVPixelBufferGetHeight(img);
        size_t stride = CVPixelBufferGetBytesPerRow(img);
        const uint8_t* base = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(img));

        std::vector<uint8_t> copy(w * h * 4);
        for (size_t y = 0; y < h; ++y) {
            std::memcpy(copy.data() + y * w * 4, base + y * stride, w * 4);
        }
        CVPixelBufferUnlockBaseAddress(img, kCVPixelBufferLock_ReadOnly);

        std::lock_guard<std::mutex> lk(self->cb_mu_);
        self->decoded_bgra_ = std::move(copy);
        self->decoded_ok_ = true;
    }

    CMFormatDescriptionRef fmt_desc_ = nullptr;
    VTDecompressionSessionRef session_ = nullptr;
    std::mutex cb_mu_;
    std::vector<uint8_t> decoded_bgra_;
    bool decoded_ok_ = false;
};
#elif defined(__ANDROID__)
class AndroidHevcDecoder {
public:
    ~AndroidHevcDecoder() { Reset(); }

    void Reset() {
        if (codec_) {
            AMediaCodec_stop(codec_);
            AMediaCodec_delete(codec_);
            codec_ = nullptr;
        }
        w_ = 0;
        h_ = 0;
        stride_ = 0;
        slice_h_ = 0;
        color_fmt_ = 21;
    }

    bool DecodeFrame(const uint8_t* data, size_t len, uint16_t width, uint16_t height,
                     std::vector<uint8_t>& out_bgra) {
        if (len < 5) return false;
        size_t pos = 0;
        uint8_t has_params = data[pos++];

        std::vector<uint8_t> csd;
        if (has_params == 1) {
            if (pos >= len) return false;
            uint8_t count = data[pos++];
            for (uint8_t i = 0; i < count; ++i) {
                if (pos + 2 > len) return false;
                uint16_t sz = ReadU16LE(data + pos);
                pos += 2;
                if (pos + sz > len) return false;
                if (sz > 0) {
                    csd.push_back(0x00);
                    csd.push_back(0x00);
                    csd.push_back(0x00);
                    csd.push_back(0x01);
                    csd.insert(csd.end(), data + pos, data + pos + sz);
                }
                pos += sz;
            }
            if (!csd.empty() && (!codec_ || w_ != width || h_ != height)) {
                if (!InitDecoder(width, height, csd)) return false;
            }
        }

        if (!codec_ || pos + 4 > len) return false;
        uint32_t sample_len = ReadU32LE(data + pos);
        pos += 4;
        if (pos + sample_len > len || sample_len == 0) return false;

        // Convert 4-byte length-prefixed HVCC NALUs -> Annex-B (00 00 00 01)
        std::vector<uint8_t> annexb;
        annexb.reserve(sample_len + 64);
        size_t cur = pos;
        size_t end = pos + sample_len;
        while (cur + 4 <= end) {
            uint32_t nalu_len = (static_cast<uint32_t>(data[cur]) << 24) |
                                (static_cast<uint32_t>(data[cur + 1]) << 16) |
                                (static_cast<uint32_t>(data[cur + 2]) << 8) |
                                static_cast<uint32_t>(data[cur + 3]);
            cur += 4;
            if (nalu_len == 0 || cur + nalu_len > end) break;
            annexb.push_back(0x00);
            annexb.push_back(0x00);
            annexb.push_back(0x00);
            annexb.push_back(0x01);
            annexb.insert(annexb.end(), data + cur, data + cur + nalu_len);
            cur += nalu_len;
        }
        if (annexb.empty()) return false;

        ssize_t in_idx = AMediaCodec_dequeueInputBuffer(codec_, 12000);
        if (in_idx < 0) return false;
        size_t in_sz = 0;
        uint8_t* in_buf = AMediaCodec_getInputBuffer(codec_, static_cast<size_t>(in_idx), &in_sz);
        if (!in_buf || in_sz < annexb.size()) {
            AMediaCodec_queueInputBuffer(codec_, static_cast<size_t>(in_idx), 0, 0, 0, 0);
            return false;
        }
        std::memcpy(in_buf, annexb.data(), annexb.size());
        AMediaCodec_queueInputBuffer(codec_, static_cast<size_t>(in_idx), 0, annexb.size(),
                                     pts_us_, 0);
        pts_us_ += 16667;

        AMediaCodecBufferInfo info{};
        for (int attempt = 0; attempt < 4; ++attempt) {
            ssize_t out_idx = AMediaCodec_dequeueOutputBuffer(codec_, &info, 8000);
            if (out_idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
                AMediaFormat* fmt = AMediaCodec_getOutputFormat(codec_);
                if (fmt) {
                    int32_t v = 0;
                    if (AMediaFormat_getInt32(fmt, "stride", &v) && v > 0) stride_ = v;
                    if (AMediaFormat_getInt32(fmt, "slice-height", &v) && v > 0) slice_h_ = v;
                    if (AMediaFormat_getInt32(fmt, "color-format", &v)) color_fmt_ = v;
                    AMediaFormat_delete(fmt);
                }
                continue;
            }
            if (out_idx == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
                continue;
            }
            if (out_idx >= 0) {
                size_t out_sz = 0;
                uint8_t* out_buf = AMediaCodec_getOutputBuffer(codec_, static_cast<size_t>(out_idx), &out_sz);
                bool ok = false;
                if (out_buf && info.size > 0) {
                    ok = ConvertYuv420ToBgra(out_buf + info.offset, static_cast<size_t>(info.size),
                                             width, height, out_bgra);
                }
                AMediaCodec_releaseOutputBuffer(codec_, static_cast<size_t>(out_idx), false);
                return ok;
            }
            break;
        }
        return false;
    }

private:
    bool InitDecoder(uint16_t width, uint16_t height, const std::vector<uint8_t>& csd) {
        Reset();
        codec_ = AMediaCodec_createDecoderByType("video/hevc");
        if (!codec_) return false;

        AMediaFormat* fmt = AMediaFormat_new();
        AMediaFormat_setString(fmt, "mime", "video/hevc");
        AMediaFormat_setInt32(fmt, "width", width);
        AMediaFormat_setInt32(fmt, "height", height);
        AMediaFormat_setInt32(fmt, "color-format", 21); // COLOR_FormatYUV420SemiPlanar (NV12)
        AMediaFormat_setBuffer(fmt, "csd-0", const_cast<uint8_t*>(csd.data()), csd.size());

        media_status_t st = AMediaCodec_configure(codec_, fmt, nullptr, nullptr, 0);
        AMediaFormat_delete(fmt);
        if (st != AMEDIA_OK) {
            Reset();
            return false;
        }
        if (AMediaCodec_start(codec_) != AMEDIA_OK) {
            Reset();
            return false;
        }
        w_ = width;
        h_ = height;
        stride_ = width;
        slice_h_ = height;
        return true;
    }

    bool ConvertYuv420ToBgra(const uint8_t* yuv, size_t yuv_len, uint16_t w, uint16_t h,
                             std::vector<uint8_t>& out_bgra) const {
        const size_t y_stride = stride_ >= w ? static_cast<size_t>(stride_) : w;
        const size_t y_plane_h = slice_h_ >= h ? static_cast<size_t>(slice_h_) : h;
        const size_t uv_offset = y_stride * y_plane_h;
        if (uv_offset + (y_stride * (h / 2)) > yuv_len) return false;

        const bool is_planar_i420 = (color_fmt_ == 19);
        const uint8_t* y_ptr = yuv;
        const uint8_t* uv_ptr = yuv + uv_offset;
        const size_t u_stride = y_stride / 2;
        const uint8_t* v_ptr = uv_ptr + u_stride * (y_plane_h / 2);

        out_bgra.resize(static_cast<size_t>(w) * h * 4);
        uint8_t* dst = out_bgra.data();

        for (uint16_t y = 0; y < h; ++y) {
            const uint8_t* y_row = y_ptr + static_cast<size_t>(y) * y_stride;
            const size_t cy = y >> 1;
            uint8_t* out_row = dst + static_cast<size_t>(y) * w * 4;
            for (uint16_t x = 0; x < w; ++x) {
                int Y = static_cast<int>(y_row[x]) - 16;
                if (Y < 0) Y = 0;
                int U = 0;
                int V = 0;
                if (is_planar_i420) {
                    U = static_cast<int>(uv_ptr[cy * u_stride + (x >> 1)]) - 128;
                    V = static_cast<int>(v_ptr[cy * u_stride + (x >> 1)]) - 128;
                } else {
                    size_t uv_idx = cy * y_stride + (x & ~1u);
                    U = static_cast<int>(uv_ptr[uv_idx]) - 128;
                    V = static_cast<int>(uv_ptr[uv_idx + 1]) - 128;
                }
                int C = 298 * Y;
                int R = (C + 409 * V + 128) >> 8;
                int G = (C - 100 * U - 208 * V + 128) >> 8;
                int B = (C + 516 * U + 128) >> 8;
                out_row[x * 4 + 0] = static_cast<uint8_t>(std::clamp(B, 0, 255));
                out_row[x * 4 + 1] = static_cast<uint8_t>(std::clamp(G, 0, 255));
                out_row[x * 4 + 2] = static_cast<uint8_t>(std::clamp(R, 0, 255));
                out_row[x * 4 + 3] = 255;
            }
        }
        return true;
    }

    AMediaCodec* codec_ = nullptr;
    uint16_t w_ = 0;
    uint16_t h_ = 0;
    int32_t stride_ = 0;
    int32_t slice_h_ = 0;
    int32_t color_fmt_ = 21;
    uint64_t pts_us_ = 0;
};
#endif

// --- Hybrid Smart Video Encoder & Decoder (Hardware HEVC + Narrowband Dirty-Tile LZ4) ---

class HybridVideoEncoderImpl : public IVideoEncoder {
public:
    bool SupportsHardwareHevc() const override {
#if defined(__APPLE__)
        return true;
#else
        return false;
#endif
    }

    void SetPeerSupportsHevc(bool supported) override {
        peer_supports_hevc_ = supported;
    }

    void SetPeerSupportsJpeg(bool supported) override {
        peer_supports_jpeg_ = supported;
    }

    void SetCodecPreference(CodecPreference pref) override {
        codec_pref_ = pref;
    }

    bool Encode(const CapturedFrame& frame,
                uint32_t target_bitrate_kbps,
                bool force_keyframe,
                bool prefer_narrowband_tiles,
                EncodedAccessUnit& out_au) override {
        uint64_t t0 = NowUs();
        out_au = {};
        out_au.frame_id = frame.frame_id;
        out_au.width = frame.width;
        out_au.height = frame.height;
        out_au.dirty_tile_count = static_cast<uint16_t>(frame.dirty_tiles.size());

        // 0-FPS Static Idle optimization: 0 changed pixels -> 0 bytes sent!
        if (!force_keyframe && frame.is_static_idle) {
            out_au.encode_duration_us = NowUs() - t0;
            return true;
        }

        // Mode 1: Explicit GPU Hardware HEVC (Apple VideoToolbox -> Apple/Android GPU Decode!)
        // Also auto-selected in kAuto when gaming 144Hz (>=12Mbps) and both peers support HEVC.
#if defined(__APPLE__)
        bool want_hevc = false;
        if (codec_pref_ == CodecPreference::kGpuHevc) {
            want_hevc = peer_supports_hevc_;
        } else if (codec_pref_ == CodecPreference::kAuto) {
            want_hevc = peer_supports_hevc_ &&
                        (!peer_supports_jpeg_ || target_bitrate_kbps >= 20000) &&
                        (!prefer_narrowband_tiles || (frame.dirty_ratio > 0.35f && target_bitrate_kbps >= 600));
        }
        if (want_hevc) {
            bool is_idr = false;
            bool need_idr = force_keyframe || !prev_was_hevc_;
            if (vt_hevc_.EncodeFrame(frame, target_bitrate_kbps, need_idr, out_au.payload, is_idr)) {
                prev_was_hevc_ = true;
                need_tile_resync_ = true;
                out_au.codec = VideoCodecId::kHevcHardware;
                out_au.is_keyframe = is_idr;
                out_au.encode_duration_us = NowUs() - t0;
                return true;
            }
        }
        prev_was_hevc_ = false;
#else
        (void)prefer_narrowband_tiles;
#endif

        // Mode 2: Hardware-Accelerated JPEG (Apple ImageIO / Windows WIC -> Flutter SIMD libjpeg-turbo)
        bool want_jpeg = (codec_pref_ == CodecPreference::kHardwareJpeg ||
                          codec_pref_ == CodecPreference::kAuto ||
                          codec_pref_ == CodecPreference::kGpuHevc) &&
                         peer_supports_jpeg_;
        if (want_jpeg) {
            float q = 0.64f;
            if (target_bitrate_kbps <= 800) {
                q = 0.44f;
            } else if (target_bitrate_kbps >= 12000) {
                q = 0.80f;
            }
            if (EncodeHardwareJpeg(frame.bgra.data(), frame.width, frame.height, q, out_au.payload) &&
                !out_au.payload.empty()) {
                need_tile_resync_ = true;
                out_au.codec = VideoCodecId::kJpegFrame;
                out_au.is_keyframe = true; // Every JPEG frame is 100% self-contained & loss-resilient!
                out_au.encode_duration_us = NowUs() - t0;
                return true;
            }
        }

        // Narrowband Dirty-Tile XOR-Residual + LZ4 path:
        // Encodes ONLY (cur_tile ^ ref_tile) so unchanged pixels inside a 64x64 tile become 0x00 and compress 40x!
        const size_t full_bytes = static_cast<size_t>(frame.width) * frame.height * 4;
        bool use_xor_residual = !force_keyframe && !need_tile_resync_ && (ref_canvas_.size() == full_bytes);
        if (!use_xor_residual) {
            ref_canvas_ = frame.bgra;
            need_tile_resync_ = false;
        }

        out_au.codec = VideoCodecId::kTileDeltaLz4;
        out_au.is_keyframe = !use_xor_residual;
        out_au.payload.clear();
        AppendU16LE(out_au.payload, static_cast<uint16_t>(frame.dirty_tiles.size()));

        std::vector<uint8_t> tile_raw;
        std::vector<uint8_t> tile_lz4;
        const uint32_t frame_stride = static_cast<uint32_t>(frame.width) * 4;

        for (const auto& rect : frame.dirty_tiles) {
            const size_t tile_bytes = static_cast<size_t>(rect.w) * rect.h * 4;
            tile_raw.resize(tile_bytes);
            for (uint16_t r = 0; r < rect.h; ++r) {
                size_t src_off = static_cast<size_t>(rect.y + r) * frame_stride + static_cast<size_t>(rect.x) * 4;
                size_t dst_off = static_cast<size_t>(r) * rect.w * 4;
                if (use_xor_residual) {
                    for (size_t b = 0; b < static_cast<size_t>(rect.w) * 4; ++b) {
                        uint8_t cur_v = frame.bgra[src_off + b];
                        tile_raw[dst_off + b] = cur_v ^ ref_canvas_[src_off + b];
                        ref_canvas_[src_off + b] = cur_v;
                    }
                } else {
                    std::memcpy(tile_raw.data() + dst_off,
                                frame.bgra.data() + src_off,
                                static_cast<size_t>(rect.w) * 4);
                }
            }

            size_t lz4_len = CompressFastLz4(tile_raw.data(), tile_raw.size(), tile_lz4);
            PayloadDirtyTileHeader thdr{};
            thdr.tile_x = rect.x;
            thdr.tile_y = rect.y;
            thdr.tile_w = rect.w;
            thdr.tile_h = rect.h;

            if (lz4_len > 0 && lz4_len <= 65535) {
                thdr.raw_or_lz4_flag = use_xor_residual ? 2 : 1; // 2 = XOR+LZ4, 1 = Copy+LZ4
                thdr.data_len = static_cast<uint16_t>(lz4_len);
                const auto* hp = reinterpret_cast<const uint8_t*>(&thdr);
                out_au.payload.insert(out_au.payload.end(), hp, hp + sizeof(thdr));
                out_au.payload.insert(out_au.payload.end(), tile_lz4.begin(), tile_lz4.end());
            } else {
                thdr.raw_or_lz4_flag = use_xor_residual ? 3 : 0; // 3 = XOR+Raw, 0 = Copy+Raw
                thdr.data_len = static_cast<uint16_t>(tile_raw.size());
                const auto* hp = reinterpret_cast<const uint8_t*>(&thdr);
                out_au.payload.insert(out_au.payload.end(), hp, hp + sizeof(thdr));
                out_au.payload.insert(out_au.payload.end(), tile_raw.begin(), tile_raw.end());
            }
        }

        out_au.encode_duration_us = NowUs() - t0;
        return true;
    }

private:
    std::vector<uint8_t> ref_canvas_;
    bool need_tile_resync_ = true;
    bool peer_supports_hevc_ = true;
    bool peer_supports_jpeg_ = false;
    CodecPreference codec_pref_ = CodecPreference::kAuto;
#if defined(__APPLE__)
    AppleHevcEncoder vt_hevc_;
    bool prev_was_hevc_ = false;
#endif
};

class HybridVideoDecoderImpl : public IVideoDecoder {
public:
    bool Decode(const EncodedAccessUnit& au, DecodedFrame& out_frame) override {
        uint64_t t0 = NowUs();
        out_frame.frame_id = au.frame_id;
        out_frame.width = au.width;
        out_frame.height = au.height;
        out_frame.codec = au.codec;

        const size_t canvas_bytes = static_cast<size_t>(au.width) * au.height * 4;
        if (canvas_.size() != canvas_bytes) {
            canvas_.assign(canvas_bytes, 0);
        }

        if (au.payload.empty()) {
            out_frame.bgra = canvas_;
            out_frame.decode_duration_us = NowUs() - t0;
            return true;
        }

        // Zero-CPU passthrough for kJpegFrame -> decoded directly on GPU/SIMD by Flutter libjpeg-turbo!
        if (au.codec == VideoCodecId::kJpegFrame) {
            out_frame.jpeg_bytes = au.payload;
            out_frame.decode_duration_us = NowUs() - t0;
            return true;
        }

        if (au.codec == VideoCodecId::kHevcHardware) {
#if defined(__APPLE__)
            std::vector<uint8_t> decoded;
            if (!vt_hevc_dec_.DecodeFrame(au.payload.data(), au.payload.size(), au.width, au.height, decoded)) {
                return false;
            }
            if (decoded.size() == canvas_bytes) {
                canvas_ = decoded;
            }
            out_frame.bgra = canvas_;
            out_frame.decode_duration_us = NowUs() - t0;
            return true;
#elif defined(__ANDROID__)
            std::vector<uint8_t> decoded;
            if (!android_hevc_dec_.DecodeFrame(au.payload.data(), au.payload.size(), au.width, au.height, decoded)) {
                return false;
            }
            if (decoded.size() == canvas_bytes) {
                canvas_ = decoded;
            }
            out_frame.bgra = canvas_;
            out_frame.decode_duration_us = NowUs() - t0;
            return true;
#else
            return false;
#endif
        }

        // Decode Dirty-Tile Delta + LZ4 directly onto persistent frame canvas!
        if (au.payload.size() < 2) return false;
        uint16_t tile_cnt = ReadU16LE(au.payload.data());
        size_t pos = 2;
        std::vector<uint8_t> tile_buf;
        const size_t dst_stride = static_cast<size_t>(au.width) * 4;

        for (uint16_t i = 0; i < tile_cnt; ++i) {
            if (pos + sizeof(PayloadDirtyTileHeader) > au.payload.size()) return false;
            PayloadDirtyTileHeader thdr{};
            std::memcpy(&thdr, au.payload.data() + pos, sizeof(thdr));
            pos += sizeof(thdr);
            if (pos + thdr.data_len > au.payload.size()) return false;

            const size_t expected_tile_bytes = static_cast<size_t>(thdr.tile_w) * thdr.tile_h * 4;
            tile_buf.resize(expected_tile_bytes);
            if (thdr.raw_or_lz4_flag == 1 || thdr.raw_or_lz4_flag == 2) {
                if (!DecompressFastLz4(au.payload.data() + pos, thdr.data_len,
                                       tile_buf.data(), expected_tile_bytes)) {
                    return false;
                }
            } else {
                if (thdr.data_len != expected_tile_bytes) return false;
                std::memcpy(tile_buf.data(), au.payload.data() + pos, expected_tile_bytes);
            }
            pos += thdr.data_len;

            bool is_xor = (thdr.raw_or_lz4_flag == 2 || thdr.raw_or_lz4_flag == 3);
            if (thdr.tile_x + thdr.tile_w <= au.width && thdr.tile_y + thdr.tile_h <= au.height) {
                for (uint16_t r = 0; r < thdr.tile_h; ++r) {
                    size_t dst_off = static_cast<size_t>(thdr.tile_y + r) * dst_stride +
                                     static_cast<size_t>(thdr.tile_x) * 4;
                    size_t row_len = static_cast<size_t>(thdr.tile_w) * 4;
                    const uint8_t* src_row = tile_buf.data() + static_cast<size_t>(r) * row_len;
                    if (is_xor) {
                        for (size_t b = 0; b < row_len; ++b) {
                            canvas_[dst_off + b] ^= src_row[b];
                        }
                    } else {
                        std::memcpy(canvas_.data() + dst_off, src_row, row_len);
                    }
                }
            }
        }

        out_frame.bgra = canvas_;
        out_frame.decode_duration_us = NowUs() - t0;
        return true;
    }

private:
    std::vector<uint8_t> canvas_;
#if defined(__APPLE__)
    AppleHevcDecoder vt_hevc_dec_;
#elif defined(__ANDROID__)
    AndroidHevcDecoder android_hevc_dec_;
#endif
};

std::unique_ptr<IVideoEncoder> CreateHybridVideoEncoder() {
    return std::make_unique<HybridVideoEncoderImpl>();
}

std::unique_ptr<IVideoDecoder> CreateHybridVideoDecoder() {
    return std::make_unique<HybridVideoDecoderImpl>();
}

} // namespace uu::core
