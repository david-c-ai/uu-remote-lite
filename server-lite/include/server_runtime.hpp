#pragma once

#include "server_protocol.hpp"
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace uu::server {

struct ServerStats {
    uint64_t registered_peers = 0;
    uint64_t punch_requests = 0;
    uint64_t active_relay_sessions = 0;
    uint64_t relayed_packets = 0;
    uint64_t relayed_bytes = 0;
};

struct ServerConfig {
    uint16_t rendezvous_port = kDefaultRendezvousPort;
    uint16_t relay_port = kDefaultRelayPort;
    uint64_t peer_ttl_ms = 60000;
    uint64_t relay_ttl_ms = 120000;
    uint64_t max_relay_kbps_per_session = 0; // 0 = unlimited
};

// Ultra-lightweight combined hbbs (Rendezvous/UDP Hole-Punching) + hbbr (Zero-Copy UDP Relay)
class LiteServer {
public:
    explicit LiteServer(const ServerConfig& cfg = {});
    ~LiteServer();

    bool Start();
    void Stop();

    uint16_t BoundRendezvousPort() const { return bound_hbbs_port_; }
    uint16_t BoundRelayPort() const { return bound_hbbr_port_; }
    ServerStats GetStats() const;

    // Authorize a relay token directly between hbbs and hbbr
    void AuthorizeRelayToken(uint64_t token);

private:
    struct PeerRecord {
        std::string id;
        uint8_t pubkey[32]{};
        EndpointV4 wan_ep{};
        EndpointV4 lan_ep{};
        uint32_t capabilities = 0;
        uint64_t last_seen_ms = 0;
    };

    struct RelaySession {
        uint64_t token = 0;
        EndpointV4 host_ep{};
        EndpointV4 client_ep{};
        bool has_host = false;
        bool has_client = false;
        uint64_t last_active_ms = 0;
        uint64_t bytes_forwarded = 0;
    };

    void RunRendezvousLoop();
    void RunRelayLoop();
    void CleanupExpired(uint64_t now_ms);

    ServerConfig cfg_;
    int hbbs_sock_ = -1;
    int hbbr_sock_ = -1;
    uint16_t bound_hbbs_port_ = 0;
    uint16_t bound_hbbr_port_ = 0;
    std::atomic<bool> running_{false};
    std::thread hbbs_thread_;
    std::thread hbbr_thread_;

    mutable std::mutex mu_;
    std::unordered_map<std::string, PeerRecord> peers_;
    std::unordered_map<uint64_t, RelaySession> relay_sessions_;

    std::atomic<uint64_t> punch_requests_{0};
    std::atomic<uint64_t> relayed_packets_{0};
    std::atomic<uint64_t> relayed_bytes_{0};
};

} // namespace uu::server
