#include "fec_qos.hpp"
#include <algorithm>
#include <cstring>

namespace uu::core {

void QoSScheduler::Enqueue(QoSLane lane, PacketType type, std::vector<uint8_t>&& frame_bytes, uint32_t reliable_seq) {
    std::lock_guard<std::mutex> lk(mu_);
    QueuedPacket pkt;
    pkt.lane = lane;
    pkt.type = type;
    pkt.reliable_seq = reliable_seq;
    pkt.wire_bytes = std::move(frame_bytes);

    if (lane == QoSLane::kP0_FastInput) {
        // Bound P0 queue to prevent stale mouse motion buildup
        if (lane_p0_.size() >= 64) {
            lane_p0_.pop_front();
        }
        lane_p0_.push_back(std::move(pkt));
    } else if (lane == QoSLane::kP1_Media) {
        // Allow up to 8192 shards so high-resolution keyframes never self-truncate
        if (lane_p1_.size() >= 8192) {
            lane_p1_.pop_front();
        }
        lane_p1_.push_back(std::move(pkt));
    } else {
        if (reliable_seq != 0) {
            unacked_p2_[reliable_seq] = pkt;
        }
        lane_p2_.push_back(std::move(pkt));
    }
}

bool QoSScheduler::PopNext(std::vector<uint8_t>& out_bytes) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!lane_p0_.empty()) {
        out_bytes = std::move(lane_p0_.front().wire_bytes);
        lane_p0_.pop_front();
        return true;
    }
    if (!lane_p1_.empty()) {
        out_bytes = std::move(lane_p1_.front().wire_bytes);
        lane_p1_.pop_front();
        return true;
    }
    if (!lane_p2_.empty()) {
        out_bytes = std::move(lane_p2_.front().wire_bytes);
        lane_p2_.pop_front();
        return true;
    }
    return false;
}

void QoSScheduler::ClearMediaQueue() {
    std::lock_guard<std::mutex> lk(mu_);
    lane_p0_.clear();
    lane_p1_.clear();
}

void QoSScheduler::AckReliable(uint32_t reliable_seq) {
    std::lock_guard<std::mutex> lk(mu_);
    unacked_p2_.erase(reliable_seq);
}

size_t QoSScheduler::PumpRetransmissions(uint64_t now_ms, uint64_t rto_ms) {
    std::lock_guard<std::mutex> lk(mu_);
    size_t retransmitted = 0;
    for (auto& kv : unacked_p2_) {
        if (kv.second.last_sent_ms == 0) {
            kv.second.last_sent_ms = now_ms;
            continue;
        }
        if (now_ms - kv.second.last_sent_ms >= rto_ms) {
            kv.second.last_sent_ms = now_ms;
            lane_p2_.push_back(kv.second);
            ++retransmitted;
        }
    }
    return retransmitted;
}

size_t QoSScheduler::PendingCount() const {
    std::lock_guard<std::mutex> lk(mu_);
    return lane_p0_.size() + lane_p1_.size() + lane_p2_.size();
}

size_t QoSScheduler::PendingMediaCount() const {
    std::lock_guard<std::mutex> lk(mu_);
    return lane_p1_.size();
}

uint16_t AdaptiveFecCodec::RecommendGroupSize(float loss_pct) {
    if (loss_pct < 0.8f) return 0; // Zero overhead on clean links!
    if (loss_pct < 4.5f) return 6; // ~16% parity overhead
    if (loss_pct < 12.0f) return 3; // ~33% parity overhead for heavy cellular/Wi-Fi packet loss
    return 2; // 50% parity overhead for extreme 15%+ loss
}

std::vector<AdaptiveFecCodec::FecParityShard> AdaptiveFecCodec::GenerateParity(
    const std::vector<std::vector<uint8_t>>& data_shards,
    uint16_t group_size) {
    std::vector<FecParityShard> out;
    if (group_size == 0 || data_shards.empty()) return out;

    const size_t total = data_shards.size();
    for (size_t start = 0; start < total; start += group_size) {
        size_t count = std::min<size_t>(group_size, total - start);
        size_t max_len = 0;
        for (size_t i = 0; i < count; ++i) {
            max_len = std::max(max_len, data_shards[start + i].size());
        }

        // Prefix each shard's 2-byte exact length into the parity calculation so variable-size last shard restores cleanly
        FecParityShard p;
        p.group_start_idx = static_cast<uint16_t>(start);
        p.group_count = static_cast<uint16_t>(count);
        p.parity_bytes.assign(max_len + 2, 0);

        for (size_t i = 0; i < count; ++i) {
            const auto& ds = data_shards[start + i];
            uint16_t len16 = static_cast<uint16_t>(ds.size());
            p.parity_bytes[0] ^= static_cast<uint8_t>(len16 & 0xFF);
            p.parity_bytes[1] ^= static_cast<uint8_t>((len16 >> 8) & 0xFF);
            for (size_t b = 0; b < ds.size(); ++b) {
                p.parity_bytes[2 + b] ^= ds[b];
            }
        }
        out.push_back(std::move(p));
    }
    return out;
}

bool AdaptiveFecCodec::TryReconstructFrame(
    uint16_t total_data_shards,
    std::vector<std::optional<std::vector<uint8_t>>>& shards,
    const std::vector<FecParityShard>& received_parities) {
    if (shards.size() < total_data_shards) {
        shards.resize(total_data_shards);
    }

    // Attempt XOR recovery on any parity group missing exactly 1 shard
    for (const auto& p : received_parities) {
        if (p.group_start_idx + p.group_count > total_data_shards) continue;
        if (p.parity_bytes.size() < 2) continue;

        int missing_idx = -1;
        int missing_cnt = 0;
        for (uint16_t i = 0; i < p.group_count; ++i) {
            if (!shards[p.group_start_idx + i].has_value()) {
                missing_idx = static_cast<int>(p.group_start_idx + i);
                ++missing_cnt;
            }
        }

        if (missing_cnt == 1 && missing_idx >= 0) {
            std::vector<uint8_t> recovered = p.parity_bytes;
            for (uint16_t i = 0; i < p.group_count; ++i) {
                int idx = static_cast<int>(p.group_start_idx + i);
                if (idx == missing_idx) continue;
                const auto& ds = *shards[idx];
                uint16_t len16 = static_cast<uint16_t>(ds.size());
                recovered[0] ^= static_cast<uint8_t>(len16 & 0xFF);
                recovered[1] ^= static_cast<uint8_t>((len16 >> 8) & 0xFF);
                size_t lim = std::min(ds.size(), recovered.size() - 2);
                for (size_t b = 0; b < lim; ++b) {
                    recovered[2 + b] ^= ds[b];
                }
            }
            uint16_t orig_len = static_cast<uint16_t>(recovered[0]) |
                                (static_cast<uint16_t>(recovered[1]) << 8);
            if (static_cast<size_t>(orig_len) + 2 <= recovered.size()) {
                shards[missing_idx] = std::vector<uint8_t>(
                    recovered.begin() + 2, recovered.begin() + 2 + orig_len);
            }
        }
    }

    for (uint16_t i = 0; i < total_data_shards; ++i) {
        if (!shards[i].has_value()) return false;
    }
    return true;
}

EncoderTargetParams NarrowbandController::Evaluate(const NetworkFeedback& net, float dirty_ratio) {
    EncoderTargetParams out{};
    out.fec_group_size = AdaptiveFecCodec::RecommendGroupSize(net.loss_pct);

    if (profile_ == QualityProfile::kUltraSaver300K) {
        // Ultra-low bandwidth profile: 250Kbps ~ 800Kbps ceiling
        out.target_bitrate_kbps = (net.loss_pct > 5.0f || net.rtt_ms > 120.0f) ? 280 : 650;
        out.max_fps = (net.rtt_ms > 100.0f) ? 24 : 30;
        out.resolution_scale = (dirty_ratio > 0.4f) ? 0.65f : 1.0f; // Keep 1.0 sharp scale for static/typing!
        out.prefer_tile_lz4 = (dirty_ratio <= 0.30f);
        return out;
    }

    if (profile_ == QualityProfile::kUltraGaming144) {
        if (net.loss_pct < 2.0f && net.rtt_ms < 25.0f) {
            smoothed_bitrate_kbps_ = std::min(80000.0f, smoothed_bitrate_kbps_ * 1.08f + 1000.0f);
        } else if (net.loss_pct > 4.0f) {
            smoothed_bitrate_kbps_ = std::max(4000.0f, smoothed_bitrate_kbps_ * 0.78f);
        } else {
            smoothed_bitrate_kbps_ = std::clamp(smoothed_bitrate_kbps_, 12000.0f, 50000.0f);
        }
        out.target_bitrate_kbps = static_cast<uint32_t>(smoothed_bitrate_kbps_);
        out.max_fps = 144;
        out.resolution_scale = 1.0f;
        out.prefer_tile_lz4 = (dirty_ratio > 0.0f && dirty_ratio <= 0.08f);
        return out;
    }

    // kBalancedAuto: AIMD (Additive Increase, Multiplicative Decrease) like WebRTC GCC
    if (net.loss_pct > 6.0f || net.rtt_ms > 150.0f) {
        smoothed_bitrate_kbps_ = std::max(350.0f, smoothed_bitrate_kbps_ * 0.72f);
    } else if (net.loss_pct < 1.5f && net.jitter_ms < 12.0f) {
        smoothed_bitrate_kbps_ = std::min(25000.0f, smoothed_bitrate_kbps_ * 1.06f + 200.0f);
    }

    out.target_bitrate_kbps = static_cast<uint32_t>(smoothed_bitrate_kbps_);
    out.max_fps = (smoothed_bitrate_kbps_ < 1200.0f) ? 30 : 60;
    out.resolution_scale = (smoothed_bitrate_kbps_ < 900.0f && dirty_ratio > 0.35f) ? 0.75f : 1.0f;
    // Key hybrid optimization: use lossless Tile-Delta LZ4 when <25% screen tiles changed (office/coding)
    out.prefer_tile_lz4 = (dirty_ratio <= 0.25f);
    return out;
}

} // namespace uu::core
