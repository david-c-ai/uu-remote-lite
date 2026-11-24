#pragma once

#include "wire_proto.hpp"
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace uu::core {

// Outbound packet queued in 3-tier QoS scheduler
struct QueuedPacket {
    QoSLane lane = QoSLane::kP1_Media;
    PacketType type = PacketType::kVideoShard;
    uint32_t reliable_seq = 0;
    uint64_t last_sent_ms = 0;
    std::vector<uint8_t> wire_bytes; // Full encrypted wire frame ready for socket send
};

// Strict Priority Scheduler (P0 Input > P1 Media > P2 Bulk File/Clipboard) + Reliable P2 Retransmission
class QoSScheduler {
public:
    void Enqueue(QoSLane lane, PacketType type, std::vector<uint8_t>&& frame_bytes, uint32_t reliable_seq = 0);

    // Pops highest-priority packet first; P0 never waits behind P1 video bursts or P2 file chunks!
    bool PopNext(std::vector<uint8_t>& out_bytes);

    // Acknowledge P2 reliable packet
    void AckReliable(uint32_t reliable_seq);

    // Re-queue unacknowledged P2 packets older than rto_ms
    size_t PumpRetransmissions(uint64_t now_ms, uint64_t rto_ms);

    size_t PendingCount() const;

private:
    mutable std::mutex mu_;
    std::deque<QueuedPacket> lane_p0_;
    std::deque<QueuedPacket> lane_p1_;
    std::deque<QueuedPacket> lane_p2_;
    std::unordered_map<uint32_t, QueuedPacket> unacked_p2_;
};

// Lightweight Block-XOR / Galois-Stripe Adaptive FEC for 0-RTT Video Packet Loss Recovery
class AdaptiveFecCodec {
public:
    // Computes parity shards based on current observed packet loss percentage:
    // loss < 1%  -> 0 parity shards (0% bandwidth overhead!)
    // loss 1..5% -> 1 parity per 8 data shards (12.5% overhead)
    // loss > 5%  -> 1 parity per 4 data shards (25% overhead, recovers bursts/single drops instantly)
    static uint16_t RecommendGroupSize(float loss_pct);

    struct FecParityShard {
        uint16_t group_start_idx;
        uint16_t group_count;
        std::vector<uint8_t> parity_bytes; // XOR parity across shards [group_start_idx .. group_start_idx + group_count)
    };

    static std::vector<FecParityShard> GenerateParity(
        const std::vector<std::vector<uint8_t>>& data_shards,
        uint16_t group_size);

    // Reconstructs missing data shards in-place if at most 1 shard is missing per parity group.
    // Returns true if ALL data shards in the frame are now present!
    static bool TryReconstructFrame(
        uint16_t total_data_shards,
        std::vector<std::optional<std::vector<uint8_t>>>& shards,
        const std::vector<FecParityShard>& received_parities);
};

// Narrowband Bandwidth & Congestion Estimator (300Kbps ~ 100Mbps Adaptive Controller)
struct NetworkFeedback {
    float rtt_ms = 8.0f;
    float jitter_ms = 1.0f;
    float loss_pct = 0.0f;
};

struct EncoderTargetParams {
    uint32_t target_bitrate_kbps = 4000;
    uint8_t max_fps = 60;
    float resolution_scale = 1.0f; // 0.5 ~ 1.0
    bool prefer_tile_lz4 = false;  // Prefer lossless narrowband dirty-tile mode on static/low-motion desktop
    uint16_t fec_group_size = 0;   // 0 = FEC off
};

class NarrowbandController {
public:
    void SetProfile(QualityProfile profile) { profile_ = profile; }
    QualityProfile GetProfile() const { return profile_; }

    EncoderTargetParams Evaluate(const NetworkFeedback& net, float dirty_ratio);

private:
    QualityProfile profile_ = QualityProfile::kBalancedAuto;
    float smoothed_bitrate_kbps_ = 6000.0f;
};

} // namespace uu::core
