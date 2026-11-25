#pragma once

#include "capture_hal.hpp"
#include "wire_proto.hpp"
#include <cstdint>
#include <memory>
#include <vector>

namespace uu::core {

struct EncodedAccessUnit {
    uint32_t frame_id = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    VideoCodecId codec = VideoCodecId::kTileDeltaLz4;
    bool is_keyframe = false;
    uint16_t dirty_tile_count = 0;
    uint64_t encode_duration_us = 0;
    std::vector<uint8_t> payload; // Empty if frame is static idle (0 Kbps!)
};

struct DecodedFrame {
    uint32_t frame_id = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    VideoCodecId codec = VideoCodecId::kTileDeltaLz4;
    uint64_t decode_duration_us = 0;
    std::vector<uint8_t> bgra;        // Populated for TileDeltaLz4 / Hardware HEVC
    std::vector<uint8_t> jpeg_bytes;  // Populated zero-copy for kJpegFrame (decoded by Flutter SIMD libjpeg-turbo)
};

// Fast cross-platform compression & hardware JPEG encoder (Apple ImageIO / Windows WIC)
size_t CompressFastLz4(const uint8_t* src, size_t src_len, std::vector<uint8_t>& out_dst);
bool DecompressFastLz4(const uint8_t* src, size_t src_len, uint8_t* dst, size_t expected_dst_len);
bool EncodeHardwareJpeg(const uint8_t* bgra, uint16_t width, uint16_t height,
                        float quality, std::vector<uint8_t>& out_jpeg);

enum class CodecPreference : uint8_t {
    kAuto         = 0, // Smart hybrid (Auto GPU HEVC / HW JPEG / Tile)
    kTileLossless = 1, // Force lossless Dirty-Tile RLE (crisp text/code)
    kGpuHevc      = 2, // Prefer GPU H.265 / HEVC hardware encode & decode
    kHardwareJpeg = 3, // Prefer Hardware ImageIO/WIC JPEG + libjpeg-turbo
};

class IVideoEncoder {
public:
    virtual ~IVideoEncoder() = default;
    virtual bool Encode(const CapturedFrame& frame,
                        uint32_t target_bitrate_kbps,
                        bool force_keyframe,
                        bool prefer_narrowband_tiles,
                        EncodedAccessUnit& out_au) = 0;
    virtual bool SupportsHardwareHevc() const = 0;
    virtual void SetPeerSupportsHevc(bool supported) = 0;
    virtual void SetPeerSupportsJpeg(bool supported) = 0;
    virtual void SetCodecPreference(CodecPreference pref) = 0;
};

class IVideoDecoder {
public:
    virtual ~IVideoDecoder() = default;
    virtual bool Decode(const EncodedAccessUnit& au, DecodedFrame& out_frame) = 0;
};

std::unique_ptr<IVideoEncoder> CreateHybridVideoEncoder();
std::unique_ptr<IVideoDecoder> CreateHybridVideoDecoder();

} // namespace uu::core
