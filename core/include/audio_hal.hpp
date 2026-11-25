#pragma once

#include "wire_proto.hpp"
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace uu::core {

// 4:1 Stereo IMA-ADPCM Compressor/Decompressor (0ms algorithmic delay, 10ms stereo chunk = 480 bytes!)
struct AdpcmChannelState {
    int16_t predictor = 0;
    uint8_t step_index = 0;
};

class AdpcmStereoCodec {
public:
    // Encodes interleaved stereo int16 PCM [L0, R0, L1, R1, ...] -> packed 4-bit ADPCM bytes
    static void Encode(const int16_t* pcm_interleaved,
                       size_t stereo_frames,
                       AdpcmChannelState& st_l,
                       AdpcmChannelState& st_r,
                       std::vector<uint8_t>& out_bytes);

    // Decodes packed 4-bit ADPCM bytes -> interleaved stereo int16 PCM
    static void Decode(const uint8_t* adpcm_bytes,
                       size_t adpcm_len,
                       size_t stereo_frames,
                       AdpcmChannelState& st_l,
                       AdpcmChannelState& st_r,
                       std::vector<int16_t>& out_pcm_interleaved);
};

using AudioPcmCallback = std::function<void(const int16_t* pcm_stereo, size_t stereo_frames, uint32_t sample_rate)>;

class ISystemAudioCapturer {
public:
    virtual ~ISystemAudioCapturer() = default;
    virtual bool Start(AudioPcmCallback cb) = 0;
    virtual void Stop() = 0;
};

class IAudioPlayer {
public:
    virtual ~IAudioPlayer() = default;
    virtual bool Start(uint32_t sample_rate = 48000, uint8_t channels = 2) = 0;
    virtual void EnqueuePcm(const int16_t* pcm_interleaved, size_t stereo_frames) = 0;
    virtual void Stop() = 0;
};

std::unique_ptr<ISystemAudioCapturer> CreateSystemAudioCapturer();
std::unique_ptr<IAudioPlayer> CreateSystemAudioPlayer();

} // namespace uu::core
