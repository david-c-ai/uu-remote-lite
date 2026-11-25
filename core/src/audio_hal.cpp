#include "audio_hal.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#include <AudioToolbox/AudioToolbox.h>
#if TARGET_OS_OSX && defined(__OBJC__)
#include <CoreGraphics/CoreGraphics.h>
#import <CoreMedia/CoreMedia.h>
#import <Foundation/Foundation.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#endif
#elif defined(__ANDROID__)
#include <aaudio/AAudio.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <mmreg.h>
#include <mmsystem.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#endif

namespace uu::core {

namespace {

constexpr int kImaIndexTable[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8
};

constexpr int kImaStepTable[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
    724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024,
    3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

uint8_t EncodeAdpcmSample(int16_t sample, AdpcmChannelState& st) {
    int step = kImaStepTable[st.step_index];
    int diff = static_cast<int>(sample) - static_cast<int>(st.predictor);
    uint8_t nibble = 0;
    if (diff < 0) {
        nibble = 8;
        diff = -diff;
    }
    int delta = step >> 3;
    if (diff >= step) {
        nibble |= 4;
        diff -= step;
        delta += step;
    }
    if (diff >= (step >> 1)) {
        nibble |= 2;
        diff -= (step >> 1);
        delta += (step >> 1);
    }
    if (diff >= (step >> 2)) {
        nibble |= 1;
        delta += (step >> 2);
    }
    int pred = static_cast<int>(st.predictor) + ((nibble & 8) ? -delta : delta);
    st.predictor = static_cast<int16_t>(std::clamp(pred, -32768, 32767));
    int next_idx = static_cast<int>(st.step_index) + kImaIndexTable[nibble & 0x0F];
    st.step_index = static_cast<uint8_t>(std::clamp(next_idx, 0, 88));
    return nibble;
}

int16_t DecodeAdpcmSample(uint8_t nibble, AdpcmChannelState& st) {
    int step = kImaStepTable[st.step_index];
    int delta = step >> 3;
    if (nibble & 4) delta += step;
    if (nibble & 2) delta += (step >> 1);
    if (nibble & 1) delta += (step >> 2);
    int pred = static_cast<int>(st.predictor) + ((nibble & 8) ? -delta : delta);
    st.predictor = static_cast<int16_t>(std::clamp(pred, -32768, 32767));
    int next_idx = static_cast<int>(st.step_index) + kImaIndexTable[nibble & 0x0F];
    st.step_index = static_cast<uint8_t>(std::clamp(next_idx, 0, 88));
    return st.predictor;
}

#if defined(_WIN32)
inline int16_t FloatToInt16(float f) {
    float clamped = std::clamp(f, -1.0f, 1.0f);
    return static_cast<int16_t>(clamped * 32767.0f);
}
#endif

} // namespace

void AdpcmStereoCodec::Encode(const int16_t* pcm_interleaved,
                              size_t stereo_frames,
                              AdpcmChannelState& st_l,
                              AdpcmChannelState& st_r,
                              std::vector<uint8_t>& out_bytes) {
    out_bytes.resize(stereo_frames);
    for (size_t i = 0; i < stereo_frames; ++i) {
        uint8_t nl = EncodeAdpcmSample(pcm_interleaved[i * 2 + 0], st_l);
        uint8_t nr = EncodeAdpcmSample(pcm_interleaved[i * 2 + 1], st_r);
        out_bytes[i] = static_cast<uint8_t>((nl & 0x0F) | ((nr & 0x0F) << 4));
    }
}

void AdpcmStereoCodec::Decode(const uint8_t* adpcm_bytes,
                              size_t adpcm_len,
                              size_t stereo_frames,
                              AdpcmChannelState& st_l,
                              AdpcmChannelState& st_r,
                              std::vector<int16_t>& out_pcm_interleaved) {
    size_t frames = std::min(stereo_frames, adpcm_len);
    out_pcm_interleaved.resize(frames * 2);
    for (size_t i = 0; i < frames; ++i) {
        uint8_t packed = adpcm_bytes[i];
        out_pcm_interleaved[i * 2 + 0] = DecodeAdpcmSample(packed & 0x0F, st_l);
        out_pcm_interleaved[i * 2 + 1] = DecodeAdpcmSample((packed >> 4) & 0x0F, st_r);
    }
}

// ============================================================================
// Host System Audio Loopback Capturers (macOS ScreenCaptureKit / Windows WASAPI)
// ============================================================================

#if defined(__APPLE__) && TARGET_OS_OSX && defined(__OBJC__)
} // namespace uu::core

@interface DirAudioStreamDelegate : NSObject <SCStreamOutput>
@property (nonatomic, assign) uu::core::AudioPcmCallback* callbackPtr;
@end

@implementation DirAudioStreamDelegate {
    std::vector<int16_t> _accum;
}

- (void)stream:(SCStream*)stream
    didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
                   ofType:(SCStreamOutputType)type {
    if (type != SCStreamOutputTypeAudio || !sampleBuffer || !_callbackPtr || !(*_callbackPtr)) return;

    CMFormatDescriptionRef fmt = CMSampleBufferGetFormatDescription(sampleBuffer);
    if (!fmt) return;
    const AudioStreamBasicDescription* asbd = CMAudioFormatDescriptionGetStreamBasicDescription(fmt);
    if (!asbd || asbd->mChannelsPerFrame == 0) return;

    CMBlockBufferRef block_buf = nullptr;
    size_t list_sz = sizeof(AudioBufferList) + sizeof(AudioBuffer) * 4;
    std::vector<uint8_t> list_mem(list_sz, 0);
    auto* list_ptr = reinterpret_cast<AudioBufferList*>(list_mem.data());

    if (CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
            sampleBuffer, nullptr, list_ptr, list_sz, nullptr, nullptr,
            kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment, &block_buf) != noErr) {
        return;
    }

    auto toI16 = [](float f) -> int16_t {
        float c = std::clamp(f, -1.0f, 1.0f);
        return static_cast<int16_t>(c * 32767.0f);
    };

    bool is_float = (asbd->mFormatFlags & kAudioFormatFlagIsFloat) != 0;
    bool is_non_interleaved = (asbd->mFormatFlags & kAudioFormatFlagIsNonInterleaved) != 0;
    uint32_t sr = asbd->mSampleRate > 1000 ? static_cast<uint32_t>(asbd->mSampleRate) : 48000;

    if (is_float && list_ptr->mNumberBuffers >= 1) {
        if (is_non_interleaved && list_ptr->mNumberBuffers >= 2) {
            const float* ch_l = static_cast<const float*>(list_ptr->mBuffers[0].mData);
            const float* ch_r = static_cast<const float*>(list_ptr->mBuffers[1].mData);
            size_t n = list_ptr->mBuffers[0].mDataByteSize / sizeof(float);
            if (ch_l && ch_r) {
                for (size_t i = 0; i < n; ++i) {
                    _accum.push_back(toI16(ch_l[i]));
                    _accum.push_back(toI16(ch_r[i]));
                }
            }
        } else {
            const float* data = static_cast<const float*>(list_ptr->mBuffers[0].mData);
            size_t total_floats = list_ptr->mBuffers[0].mDataByteSize / sizeof(float);
            uint32_t ch = asbd->mChannelsPerFrame;
            if (data) {
                for (size_t i = 0; i + ch <= total_floats; i += ch) {
                    int16_t l = toI16(data[i]);
                    int16_t r = (ch >= 2) ? toI16(data[i + 1]) : l;
                    _accum.push_back(l);
                    _accum.push_back(r);
                }
            }
        }
    }

    if (block_buf) CFRelease(block_buf);

    constexpr size_t kChunkStereoFrames = 480; // 10ms @ 48kHz -> 480B ADPCM packet!
    while (_accum.size() >= kChunkStereoFrames * 2) {
        (*_callbackPtr)(_accum.data(), kChunkStereoFrames, sr);
        _accum.erase(_accum.begin(), _accum.begin() + kChunkStereoFrames * 2);
    }
}
@end

namespace uu::core {

class MacSckAudioCapturer : public ISystemAudioCapturer {
public:
    ~MacSckAudioCapturer() override { Stop(); }

    bool Start(AudioPcmCallback cb) override {
        Stop();
        if (!CGPreflightScreenCaptureAccess()) return false;
        cb_ = std::move(cb);

        if (@available(macOS 13.0, *)) {
            __block bool ok = false;
            dispatch_semaphore_t sem = dispatch_semaphore_create(0);
            [SCShareableContent getShareableContentWithCompletionHandler:^(
                                    SCShareableContent* content, NSError* err) {
                if (!content || err || content.displays.count == 0) {
                    dispatch_semaphore_signal(sem);
                    return;
                }
                SCDisplay* disp = content.displays.firstObject;
                SCContentFilter* filter = [[SCContentFilter alloc] initWithDisplay:disp excludingWindows:@[]];
                SCStreamConfiguration* cfg = [[SCStreamConfiguration alloc] init];
                cfg.width = 64;
                cfg.height = 64;
                cfg.minimumFrameInterval = CMTimeMake(1, 2); // Minimal video overhead
                cfg.capturesAudio = YES;
                cfg.excludesCurrentProcessAudio = YES;
                cfg.sampleRate = 48000;
                cfg.channelCount = 2;

                delegate_ = [[DirAudioStreamDelegate alloc] init];
                delegate_.callbackPtr = &cb_;
                audio_queue_ = dispatch_queue_create("com.dirremote.audio.cap", DISPATCH_QUEUE_SERIAL);
                stream_ = [[SCStream alloc] initWithFilter:filter configuration:cfg delegate:nil];

                NSError* addErr = nil;
                [stream_ addStreamOutput:delegate_
                                    type:SCStreamOutputTypeAudio
                      sampleHandlerQueue:audio_queue_
                                   error:&addErr];
                if (!addErr) {
                    [stream_ startCaptureWithCompletionHandler:^(NSError* startErr) {
                        ok = (startErr == nil);
                        dispatch_semaphore_signal(sem);
                    }];
                } else {
                    dispatch_semaphore_signal(sem);
                }
            }];
            dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 400 * NSEC_PER_MSEC));
            return ok;
        }
        return false;
    }

    void Stop() override {
        if (delegate_) {
            delegate_.callbackPtr = nullptr;
        }
        if (@available(macOS 13.0, *)) {
            if (stream_) {
                [stream_ stopCaptureWithCompletionHandler:^(NSError*) {}];
                stream_ = nil;
            }
        }
        delegate_ = nil;
        audio_queue_ = nil;
    }

private:
    AudioPcmCallback cb_;
    SCStream* stream_ = nil;
    DirAudioStreamDelegate* delegate_ = nil;
    dispatch_queue_t audio_queue_ = nil;
};
#elif defined(_WIN32)
class WinWasapiAudioCapturer : public ISystemAudioCapturer {
public:
    ~WinWasapiAudioCapturer() override { Stop(); }

    bool Start(AudioPcmCallback cb) override {
        Stop();
        cb_ = std::move(cb);
        running_.store(true);
        worker_ = std::thread(&WinWasapiAudioCapturer::CaptureLoop, this);
        return true;
    }

    void Stop() override {
        if (!running_.exchange(false)) return;
        if (worker_.joinable()) worker_.join();
    }

private:
    void CaptureLoop() {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        bool need_uninit = SUCCEEDED(hr);

        IMMDeviceEnumerator* enumerator = nullptr;
        IMMDevice* device = nullptr;
        IAudioClient* client = nullptr;
        IAudioCaptureClient* capture = nullptr;
        WAVEFORMATEX* mix_fmt = nullptr;

        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                       __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator))) &&
            SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)) &&
            SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                       reinterpret_cast<void**>(&client))) &&
            SUCCEEDED(client->GetMixFormat(&mix_fmt)) &&
            SUCCEEDED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                                         200000, 0, mix_fmt, nullptr)) &&
            SUCCEEDED(client->GetService(__uuidof(IAudioCaptureClient),
                                         reinterpret_cast<void**>(&capture))) &&
            SUCCEEDED(client->Start())) {
            std::vector<int16_t> accum;
            uint32_t sr = mix_fmt->nSamplesPerSec ? mix_fmt->nSamplesPerSec : 48000;
            uint16_t ch = mix_fmt->nChannels ? mix_fmt->nChannels : 2;
            bool is_float = (mix_fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) ||
                            (mix_fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mix_fmt->wBitsPerSample == 32);

            while (running_.load(std::memory_order_relaxed)) {
                UINT32 pkt_len = 0;
                if (FAILED(capture->GetNextPacketSize(&pkt_len)) || pkt_len == 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    continue;
                }
                while (pkt_len > 0 && running_.load(std::memory_order_relaxed)) {
                    BYTE* data = nullptr;
                    UINT32 num_frames = 0;
                    DWORD flags = 0;
                    if (SUCCEEDED(capture->GetBuffer(&data, &num_frames, &flags, nullptr, nullptr))) {
                        if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                            accum.insert(accum.end(), num_frames * 2, 0);
                        } else if (is_float && data) {
                            const float* fptr = reinterpret_cast<const float*>(data);
                            for (UINT32 i = 0; i < num_frames; ++i) {
                                int16_t l = FloatToInt16(fptr[i * ch]);
                                int16_t r = (ch >= 2) ? FloatToInt16(fptr[i * ch + 1]) : l;
                                accum.push_back(l);
                                accum.push_back(r);
                            }
                        } else if (data) {
                            const int16_t* sptr = reinterpret_cast<const int16_t*>(data);
                            for (UINT32 i = 0; i < num_frames; ++i) {
                                int16_t l = sptr[i * ch];
                                int16_t r = (ch >= 2) ? sptr[i * ch + 1] : l;
                                accum.push_back(l);
                                accum.push_back(r);
                            }
                        }
                        capture->ReleaseBuffer(num_frames);
                    }
                    constexpr size_t kChunk = 480;
                    while (accum.size() >= kChunk * 2) {
                        if (cb_) cb_(accum.data(), kChunk, sr);
                        accum.erase(accum.begin(), accum.begin() + kChunk * 2);
                    }
                    if (FAILED(capture->GetNextPacketSize(&pkt_len))) break;
                }
            }
            client->Stop();
        }

        if (mix_fmt) CoTaskMemFree(mix_fmt);
        if (capture) capture->Release();
        if (client) client->Release();
        if (device) device->Release();
        if (enumerator) enumerator->Release();
        if (need_uninit) CoUninitialize();
    }

    AudioPcmCallback cb_;
    std::atomic<bool> running_{false};
    std::thread worker_;
};
#endif

// ============================================================================
// Cross-Platform Low-Latency Audio Players (Apple AudioQueue / Android AAudio / WinMM)
// ============================================================================

#if defined(__APPLE__)
class AppleAudioQueuePlayer : public IAudioPlayer {
public:
    ~AppleAudioQueuePlayer() override { Stop(); }

    bool Start(uint32_t sample_rate, uint8_t channels) override {
        Stop();
        AudioStreamBasicDescription fmt{};
        fmt.mSampleRate = sample_rate;
        fmt.mFormatID = kAudioFormatLinearPCM;
        fmt.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
        fmt.mFramesPerPacket = 1;
        fmt.mChannelsPerFrame = channels;
        fmt.mBitsPerChannel = 16;
        fmt.mBytesPerFrame = channels * 2;
        fmt.mBytesPerPacket = channels * 2;

        if (AudioQueueNewOutput(&fmt, &AppleAudioQueuePlayer::QueueCallback, this,
                                nullptr, nullptr, 0, &queue_) != noErr || !queue_) {
            return false;
        }
        for (int i = 0; i < kNumBuffers; ++i) {
            AudioQueueAllocateBuffer(queue_, kBufBytes, &buffers_[i]);
            std::memset(buffers_[i]->mAudioData, 0, kBufBytes);
            buffers_[i]->mAudioDataByteSize = kBufBytes;
            AudioQueueEnqueueBuffer(queue_, buffers_[i], 0, nullptr);
        }
        running_.store(true);
        AudioQueueStart(queue_, nullptr);
        return true;
    }

    void EnqueuePcm(const int16_t* pcm_interleaved, size_t stereo_frames) override {
        std::lock_guard<std::mutex> lk(mu_);
        size_t samples = stereo_frames * 2;
        if (ring_.size() > 48000) {
            ring_.erase(ring_.begin(), ring_.begin() + (ring_.size() - 9600));
        }
        ring_.insert(ring_.end(), pcm_interleaved, pcm_interleaved + samples);
    }

    void Stop() override {
        running_.store(false);
        if (queue_) {
            AudioQueueStop(queue_, true);
            AudioQueueDispose(queue_, true);
            queue_ = nullptr;
        }
        std::lock_guard<std::mutex> lk(mu_);
        ring_.clear();
    }

private:
    static void QueueCallback(void* user_data, AudioQueueRef aq, AudioQueueBufferRef buf) {
        auto* self = static_cast<AppleAudioQueuePlayer*>(user_data);
        if (!self->running_.load(std::memory_order_relaxed)) return;
        size_t want_samples = kBufBytes / sizeof(int16_t);
        auto* out = static_cast<int16_t*>(buf->mAudioData);
        {
            std::lock_guard<std::mutex> lk(self->mu_);
            size_t avail = std::min(want_samples, self->ring_.size());
            for (size_t i = 0; i < avail; ++i) {
                out[i] = self->ring_.front();
                self->ring_.pop_front();
            }
            if (avail < want_samples) {
                std::memset(out + avail, 0, (want_samples - avail) * sizeof(int16_t));
            }
        }
        buf->mAudioDataByteSize = kBufBytes;
        AudioQueueEnqueueBuffer(aq, buf, 0, nullptr);
    }

    static constexpr int kNumBuffers = 4;
    static constexpr uint32_t kBufBytes = 480 * 2 * 2; // 10ms stereo int16
    AudioQueueRef queue_ = nullptr;
    AudioQueueBufferRef buffers_[kNumBuffers]{};
    std::atomic<bool> running_{false};
    std::mutex mu_;
    std::deque<int16_t> ring_;
};
#elif defined(__ANDROID__)
class AndroidAAudioPlayer : public IAudioPlayer {
public:
    ~AndroidAAudioPlayer() override { Stop(); }

    bool Start(uint32_t sample_rate, uint8_t channels) override {
        Stop();
        AAudioStreamBuilder* builder = nullptr;
        if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK || !builder) return false;
        AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
        AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
        AAudioStreamBuilder_setSampleRate(builder, static_cast<int32_t>(sample_rate));
        AAudioStreamBuilder_setChannelCount(builder, channels);
        AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
        AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
        AAudioStreamBuilder_setDataCallback(builder, &AndroidAAudioPlayer::DataCallback, this);

        aaudio_result_t res = AAudioStreamBuilder_openStream(builder, &stream_);
        AAudioStreamBuilder_delete(builder);
        if (res != AAUDIO_OK || !stream_) {
            stream_ = nullptr;
            return false;
        }
        return AAudioStream_requestStart(stream_) == AAUDIO_OK;
    }

    void EnqueuePcm(const int16_t* pcm_interleaved, size_t stereo_frames) override {
        std::lock_guard<std::mutex> lk(mu_);
        size_t samples = stereo_frames * 2;
        if (ring_.size() > 48000) {
            ring_.erase(ring_.begin(), ring_.begin() + (ring_.size() - 9600));
        }
        ring_.insert(ring_.end(), pcm_interleaved, pcm_interleaved + samples);
    }

    void Stop() override {
        if (stream_) {
            AAudioStream_requestStop(stream_);
            AAudioStream_close(stream_);
            stream_ = nullptr;
        }
        std::lock_guard<std::mutex> lk(mu_);
        ring_.clear();
    }

private:
    static aaudio_data_callback_result_t DataCallback(
        AAudioStream*, void* user_data, void* audio_data, int32_t num_frames) {
        auto* self = static_cast<AndroidAAudioPlayer*>(user_data);
        auto* out = static_cast<int16_t*>(audio_data);
        size_t want_samples = static_cast<size_t>(num_frames) * 2;
        std::lock_guard<std::mutex> lk(self->mu_);
        size_t avail = std::min(want_samples, self->ring_.size());
        for (size_t i = 0; i < avail; ++i) {
            out[i] = self->ring_.front();
            self->ring_.pop_front();
        }
        if (avail < want_samples) {
            std::memset(out + avail, 0, (want_samples - avail) * sizeof(int16_t));
        }
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }

    AAudioStream* stream_ = nullptr;
    std::mutex mu_;
    std::deque<int16_t> ring_;
};
#elif defined(_WIN32)
class WinWaveOutPlayer : public IAudioPlayer {
public:
    ~WinWaveOutPlayer() override { Stop(); }

    bool Start(uint32_t sample_rate, uint8_t channels) override {
        Stop();
        WAVEFORMATEX wf{};
        wf.wFormatTag = WAVE_FORMAT_PCM;
        wf.nChannels = channels;
        wf.nSamplesPerSec = sample_rate;
        wf.wBitsPerSample = 16;
        wf.nBlockAlign = channels * 2;
        wf.nAvgBytesPerSec = sample_rate * wf.nBlockAlign;
        if (waveOutOpen(&hwo_, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
            hwo_ = nullptr;
            return false;
        }
        return true;
    }

    void EnqueuePcm(const int16_t* pcm_interleaved, size_t stereo_frames) override {
        if (!hwo_ || stereo_frames == 0) return;
        std::lock_guard<std::mutex> lk(mu_);
        // Reclaim finished buffers
        while (!inflight_.empty() && (inflight_.front()->hdr.dwFlags & WHDR_DONE)) {
            waveOutUnprepareHeader(hwo_, &inflight_.front()->hdr, sizeof(WAVEHDR));
            inflight_.pop_front();
        }
        if (inflight_.size() > 16) return; // Drop excess to preserve low latency

        auto buf = std::make_unique<WaveBuf>();
        buf->pcm.assign(pcm_interleaved, pcm_interleaved + stereo_frames * 2);
        buf->hdr.lpData = reinterpret_cast<LPSTR>(buf->pcm.data());
        buf->hdr.dwBufferLength = static_cast<DWORD>(buf->pcm.size() * sizeof(int16_t));
        if (waveOutPrepareHeader(hwo_, &buf->hdr, sizeof(WAVEHDR)) == MMSYSERR_NOERROR) {
            waveOutWrite(hwo_, &buf->hdr, sizeof(WAVEHDR));
            inflight_.push_back(std::move(buf));
        }
    }

    void Stop() override {
        if (hwo_) {
            waveOutReset(hwo_);
            std::lock_guard<std::mutex> lk(mu_);
            for (auto& b : inflight_) {
                waveOutUnprepareHeader(hwo_, &b->hdr, sizeof(WAVEHDR));
            }
            inflight_.clear();
            waveOutClose(hwo_);
            hwo_ = nullptr;
        }
    }

private:
    struct WaveBuf {
        std::vector<int16_t> pcm;
        WAVEHDR hdr{};
    };
    HWAVEOUT hwo_ = nullptr;
    std::mutex mu_;
    std::deque<std::unique_ptr<WaveBuf>> inflight_;
};
#endif

class NullAudioCapturer : public ISystemAudioCapturer {
public:
    bool Start(AudioPcmCallback) override { return false; }
    void Stop() override {}
};

class NullAudioPlayer : public IAudioPlayer {
public:
    bool Start(uint32_t, uint8_t) override { return true; }
    void EnqueuePcm(const int16_t*, size_t) override {}
    void Stop() override {}
};

std::unique_ptr<ISystemAudioCapturer> CreateSystemAudioCapturer() {
#if defined(__APPLE__) && TARGET_OS_OSX && defined(__OBJC__)
    return std::make_unique<MacSckAudioCapturer>();
#elif defined(_WIN32)
    return std::make_unique<WinWasapiAudioCapturer>();
#else
    return std::make_unique<NullAudioCapturer>();
#endif
}

std::unique_ptr<IAudioPlayer> CreateSystemAudioPlayer() {
#if defined(__APPLE__)
    return std::make_unique<AppleAudioQueuePlayer>();
#elif defined(__ANDROID__)
    return std::make_unique<AndroidAAudioPlayer>();
#elif defined(_WIN32)
    return std::make_unique<WinWaveOutPlayer>();
#else
    return std::make_unique<NullAudioPlayer>();
#endif
}

} // namespace uu::core
