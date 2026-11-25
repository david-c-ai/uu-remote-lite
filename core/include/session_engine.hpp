#pragma once

#include "audio_hal.hpp"
#include "capture_hal.hpp"
#include "codec_hal.hpp"
#include "crypto_e2ee.hpp"
#include "fec_qos.hpp"
#include "hid_clipboard_file.hpp"
#include "keymap_hal.hpp"
#include "server_protocol.hpp"
#include "wire_proto.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace uu::core {

enum class TransportPath : uint8_t {
    kDisconnected = 0,
    kP2pDirect    = 1,
    kRelayFallback = 2,
};

struct SessionTelemetry {
    TransportPath path = TransportPath::kDisconnected;
    QualityProfile active_profile = QualityProfile::kBalancedAuto;
    VideoCodecId active_codec = VideoCodecId::kTileDeltaLz4;
    float connect_time_ms = 0.0f;
    float rtt_ms = 0.0f;
    float loss_pct = 0.0f;
    float realtime_kbps = 0.0f;
    float current_fps = 0.0f;
    float avg_encode_ms = 0.0f;
    float avg_decode_ms = 0.0f;
    uint64_t frames_sent = 0;
    uint64_t frames_decoded = 0;
    uint64_t fec_recovered_frames = 0;
    uint64_t static_idle_skips = 0;
    uint64_t cursor_updates = 0;
    uint16_t remote_width = 0;
    uint16_t remote_height = 0;
    uint16_t cursor_x_norm = 32768;
    uint16_t cursor_y_norm = 32768;
    uint8_t monitor_count = 1;
    uint8_t active_monitor = 0;
    bool privacy_curtain_on = false;
};

struct NodeConfig {
    std::string self_id = "100200300";
    std::string access_pin = "8866";
    std::string server_ip = "127.0.0.1";
    uint16_t hbbs_port = uu::server::kDefaultRendezvousPort;
    uint16_t hbbr_port = uu::server::kDefaultRelayPort;
    bool is_host = false;
    bool use_synthetic_capture = false;
    bool block_direct_p2p_for_test = false; // Simulates strict Symmetric NAT to test Happy-Eyeballs Relay
    float simulated_rx_loss_pct = 0.0f;     // Simulates weak network UDP packet drops
    HostOsType simulated_os_override = HostOsType::kUnknown; // Optional OS simulation for cross-OS E2E tests
    std::string download_dir = "/tmp";
};

class PeerNode {
public:
    explicit PeerNode(const NodeConfig& cfg);
    ~PeerNode();

    bool Start();
    void Stop();

    // Client initiates connection to remote host_id; blocks up to timeout_ms until connected
    bool ConnectToPeer(const std::string& target_host_id, uint32_t timeout_ms = 2500);

    bool IsConnected() const { return path_.load() != TransportPath::kDisconnected; }
    SessionTelemetry GetTelemetry() const;

    // Host workload control (when synthetic capturer is active)
    void SetSyntheticWorkload(SyntheticWorkload mode);

    // Client control & input APIs
    void SendCursorMoveFast(uint16_t x_norm, uint16_t y_norm, int16_t dx = 0, int16_t dy = 0,
                            uint8_t button_mask = 0, int16_t wheel_y = 0);
    void SendKey(uint16_t keycode, bool is_down, uint8_t modifiers = 0);
    void SendUnicodeText(const std::string& utf8_text);
    void SendGamepad(const PayloadGamepad& pad);
    void SetQualityProfile(QualityProfile profile, uint8_t target_fps = 60,
                           bool privacy_curtain = false, uint8_t monitor_index = 0,
                           CodecPreference codec_pref = CodecPreference::kAuto,
                           bool audio_enabled = true);
    void SendClipboardText(const std::string& utf8_text);
    bool SendFile(const std::string& local_file_path, uint64_t file_id = 1);
    void InjectSyntheticAudioFrameForTest(const int16_t* pcm_interleaved,
                                          size_t stereo_frames,
                                          uint32_t sample_rate = 48000);

    std::string LastClipboardText() const;
    bool CopyLatestFrameBgra(std::vector<uint8_t>& out_bgra, uint16_t& out_w, uint16_t& out_h) const;
    // out_format: 1 = BGRA8888 raw, 2 = JPEG compressed bitstream (decoded by Flutter SIMD libjpeg-turbo)
    bool CopyLatestFrameEx(uint64_t last_seen_seq,
                           uint64_t& out_seq,
                           uint8_t& out_format,
                           std::vector<uint8_t>& out_bytes,
                           uint16_t& out_w,
                           uint16_t& out_h) const;
    void ForceJpegModeForTest(bool enable) {
        force_jpeg_test_ = enable;
        if (enable && encoder_) {
            encoder_->SetCodecPreference(CodecPreference::kHardwareJpeg);
        }
    }
    uint64_t HostInjectedInputCount() const { return input_dispatcher_.InjectedEventCount(); }
    uint64_t HostInjectedTextCount() const { return input_dispatcher_.InjectedTextCount(); }
    std::string HostLastUnicodeText() const { return input_dispatcher_.LastUnicodeText(); }
    HostOsType HostPeerOsType() const { return input_dispatcher_.PeerOsType(); }
    uint64_t HostAudioPacketsSent() const { return audio_tx_pkts_.load(); }
    uint64_t ClientAudioPacketsReceived() const { return audio_rx_pkts_.load(); }
    bool IsLastFileVerified() const { return last_file_sha256_ok_.load(); }

private:
    struct RxFrameAssembly {
        uint16_t total_data = 0;
        uint16_t parity_cnt = 0;
        uint16_t width = 0;
        uint16_t height = 0;
        VideoCodecId codec = VideoCodecId::kTileDeltaLz4;
        bool is_keyframe = false;
        uint64_t created_ms = 0;
        bool completed = false;
        bool used_fec = false;
        std::vector<std::optional<std::vector<uint8_t>>> shards;
        std::vector<AdaptiveFecCodec::FecParityShard> parities;
    };

    void NetworkLoop();
    void HostCaptureLoop();
    void FlushScheduler();

    void SendEncrypted(QoSLane lane, PacketType type, const void* payload, size_t len, uint32_t ack_or_group = 0);
    void SendRawWireToActivePath(const uint8_t* data, size_t len);
    void SendPcmAudioFrame(const int16_t* pcm_interleaved, size_t stereo_frames, uint32_t sample_rate);
    void HandleWirePacket(const uint8_t* raw, size_t len, const uu::server::EndpointV4& sender_ep, bool via_relay);
    void HandleVideoPacket(PacketType type, const uint8_t* payload, size_t len);
    void HandleAudioPacket(const uint8_t* payload, size_t len);

    NodeConfig cfg_;
    int sock_ = -1;
    uint16_t local_port_ = 0;
    KeyPair25519 static_keys_;
    std::array<uint8_t, 32> session_key_{};
    bool has_session_key_ = false;

    uu::server::EndpointV4 hbbs_ep_{};
    uu::server::EndpointV4 hbbr_ep_{};
    uu::server::EndpointV4 peer_wan_ep_{};
    uu::server::EndpointV4 peer_lan_ep_{};
    uint64_t relay_token_ = 0;
    bool relay_bound_ = false;
    uint64_t punch_started_ms_ = 0;

    std::atomic<bool> running_{false};
    std::atomic<TransportPath> path_{TransportPath::kDisconnected};
    std::atomic<uint32_t> tx_seq_{1};
    std::atomic<uint32_t> p2_seq_{1};

    std::thread net_thread_;
    std::thread capture_thread_;

    QoSScheduler scheduler_;
    NarrowbandController bwe_;
    std::unique_ptr<IScreenCapturer> capturer_;
    SyntheticCapturer* synthetic_ptr_ = nullptr;
    std::unique_ptr<IVideoEncoder> encoder_;
    std::unique_ptr<IVideoDecoder> decoder_;
    std::unique_ptr<ISystemAudioCapturer> audio_capturer_;
    std::unique_ptr<IAudioPlayer> audio_player_;
    InputDispatcher input_dispatcher_{false};
    ResumableFileEngine file_engine_;

    std::atomic<bool> audio_enabled_{true};
    std::atomic<bool> audio_capturer_started_{false};
    std::atomic<bool> audio_player_started_{false};
    std::atomic<uint32_t> audio_tx_seq_{1};
    std::atomic<uint64_t> audio_tx_pkts_{0};
    std::atomic<uint64_t> audio_rx_pkts_{0};
    std::mutex audio_tx_mu_;
    AdpcmChannelState audio_enc_l_{};
    AdpcmChannelState audio_enc_r_{};

    mutable std::mutex state_mu_;
    SessionTelemetry telemetry_{};
    std::string last_clipboard_;
    std::vector<uint8_t> latest_bgra_;
    std::vector<uint8_t> latest_jpeg_;
    uint64_t latest_frame_seq_ = 0;
    uint8_t latest_frame_format_ = 0; // 1 = BGRA, 2 = JPEG
    uint16_t latest_w_ = 0;
    uint16_t latest_h_ = 0;
    bool force_jpeg_test_ = false;
    std::unordered_map<uint64_t, std::vector<uint8_t>> cursor_shape_cache_;
    std::unordered_map<uint32_t, RxFrameAssembly> rx_frames_;

    uint64_t bytes_window_ = 0;
    uint64_t frames_window_ = 0;
    uint64_t window_start_ms_ = 0;
    uint32_t rx_pkt_window_ = 0;
    uint32_t lost_pkt_window_ = 0;
    uint32_t last_rx_seq_ = 0;
    uint16_t last_sent_cx_ = 0;
    uint16_t last_sent_cy_ = 0;
    uint8_t prev_tx_btn_mask_ = 0;
    uint32_t last_decoded_frame_id_ = 0;
    uint64_t last_pli_sent_ms_ = 0;
    std::atomic<bool> force_next_idr_{true};
    std::atomic<bool> last_file_sha256_ok_{false};
};

} // namespace uu::core
