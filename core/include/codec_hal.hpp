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
    std::vector<uint8_t> bgra;
};

// Fast cross-platform LZ4 block wrapper (uses Apple libcompression SIMD LZ4 on macOS/iOS, portable fast LZ on Win/Android)
size_t CompressFastLz4(const uint8_t* src, size_t src_len, std::vector<uint8_t>& out_dst);
bool DecompressFastLz4(const uint8_t* src, size_t src_len, uint8_t* dst, size_t expected_dst_len);

class IVideoEncoder {
public:
    virtual ~IVideoEncoder() = default;
    virtual bool Encode(const CapturedFrame& frame,
                        uint32_t target_bitrate_kbps,
                        bool force_keyframe,
                        bool prefer_narrowband_tiles,
                        EncodedAccessUnit& out_au) = 0;
    virtual bool SupportsHardwareHevc() const = 0;
};

class IVideoDecoder {
public:
    virtual ~IVideoDecoder() = default;
    virtual bool Decode(const EncodedAccessUnit& au, DecodedFrame& out_frame) = 0;
};

std::unique_ptr<IVideoEncoder> CreateHybridVideoEncoder();
std::unique_ptr<IVideoDecoder> CreateHybridVideoDecoder();

} // namespace uu::core
