#pragma once

#include <cstdint>
#include <cstring>

namespace uu::server {

// Magic: "UUS1" (0x55555331)
constexpr uint32_t kServerMagic = 0x55555331;
constexpr uint16_t kDefaultRendezvousPort = 21116; // Compatible port layout with hbbs
constexpr uint16_t kDefaultRelayPort = 21117;      // Compatible port layout with hbbr

enum class SigMsgType : uint8_t {
    // Host <-> hbbs (UDP)
    kRegisterPeer      = 0x01, // Peer registers ID + X25519 pubkey + LAN endpoint
    kRegisterAck       = 0x02, // hbbs confirms registration + observed public IP:Port
    kHeartbeat         = 0x03, // 32-byte lightweight keepalive (every 25s)
    kHeartbeatAck      = 0x04,

    // Client -> hbbs -> Host (UDP Happy Eyeballs Broker)
    kPunchRequest      = 0x10, // Client requests connection to target_id
    kPunchOrder        = 0x11, // Sent simultaneously to BOTH Client and Host with peer endpoints + relay token
    kPeerOffline       = 0x12, // Target device offline or not registered

    // Peer -> hbbr (UDP Zero-Copy Relay Channel)
    kRelayBind         = 0x20, // Bind session_token on relay server (16-byte header)
    kRelayBindAck      = 0x21, // Relay channel paired & ready
    kRelayData         = 0x22, // Zero-copy forwarded media/control payload
};

#pragma pack(push, 1)

struct ServerHeader {
    uint32_t magic;      // kServerMagic
    SigMsgType type;     // SigMsgType
    uint8_t flags;       // Reserved / NAT type hint
    uint16_t payload_len;
};

struct EndpointV4 {
    uint32_t ip_be;      // Network byte order IPv4
    uint16_t port_be;    // Network byte order port
};

// 32-byte lightweight heartbeat packet
struct MsgHeartbeat {
    ServerHeader hdr;    // 8B
    char peer_id[12];    // Fixed 9~11 char numeric/alphanumeric ID (null-padded)
    uint64_t timestamp_ms;
    uint32_t capabilities; // Bitmask: HEVC, AV1, MultiMon, VirtualDisplay
};

struct MsgRegisterPeer {
    ServerHeader hdr;
    char peer_id[12];
    uint8_t pubkey[32];      // X25519 static public key
    EndpointV4 lan_ep;       // Local LAN IP:Port for instant LAN shortcut
    uint32_t capabilities;
};

struct MsgRegisterAck {
    ServerHeader hdr;
    EndpointV4 observed_wan_ep;
    uint64_t server_time_ms;
};

struct MsgPunchRequest {
    ServerHeader hdr;
    char client_id[12];
    char target_id[12];
    uint8_t client_pubkey[32];
    EndpointV4 client_lan_ep;
};

struct MsgPunchOrder {
    ServerHeader hdr;
    char peer_id[12];           // Remote peer ID
    uint8_t peer_pubkey[32];    // Remote peer X25519 pubkey
    EndpointV4 peer_wan_ep;     // Remote peer observed WAN IP:Port for UDP hole punching
    EndpointV4 peer_lan_ep;     // Remote peer LAN IP:Port for same-LAN 0-hop direct link
    uint16_t relay_port_be;     // hbbr UDP port for 250ms Happy-Eyeballs fallback
    uint64_t relay_token;       // One-time 64-bit token authorizing relay session
    uint8_t is_initiator;       // 1 = client, 0 = host
};

struct MsgRelayBind {
    ServerHeader hdr;
    uint64_t relay_token;
    uint8_t role;               // 1 = client, 0 = host
};

// Minimal 16-byte relay frame header prepended to encrypted payloads when relayed
struct RelayFrameHeader {
    ServerHeader hdr;           // 8B (type = kRelayData)
    uint64_t relay_token;       // 8B session routing key
};

#pragma pack(pop)

inline void CopyPeerId(char (&dst)[12], const char* src) {
    std::memset(dst, 0, sizeof(dst));
    if (src) {
        std::strncpy(dst, src, sizeof(dst) - 1);
    }
}

} // namespace uu::server
