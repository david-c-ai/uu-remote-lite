#include "server_runtime.hpp"

#include <chrono>
#include <cstring>
#include <random>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using socklen_t = int;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace uu::server {

namespace {

uint64_t NowMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

void CloseSock(int fd) {
    if (fd < 0) return;
#ifdef _WIN32
    closesocket(fd);
#else
    close(fd);
#endif
}

int CreateBoundUdpSocket(uint16_t requested_port, uint16_t& out_bound_port) {
#ifdef _WIN32
    static bool wsa_init = []() {
        WSADATA wsa;
        return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    }();
    (void)wsa_init;
#endif
    int fd = static_cast<int>(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    if (fd < 0) return -1;

    int reuse = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    int buf_sz = 4 * 1024 * 1024;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&buf_sz), sizeof(buf_sz));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&buf_sz), sizeof(buf_sz));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(requested_port);

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        CloseSock(fd);
        return -1;
    }

    sockaddr_in bound{};
    socklen_t len = sizeof(bound);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &len) == 0) {
        out_bound_port = ntohs(bound.sin_port);
    } else {
        out_bound_port = requested_port;
    }

#ifdef _WIN32
    u_long nonblock = 1;
    ioctlsocket(fd, FIONBIO, &nonblock);
#else
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
    return fd;
}

void SendUdpTo(int fd, const EndpointV4& ep, const void* data, size_t len) {
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = ep.ip_be;
    dst.sin_port = ep.port_be;
    ::sendto(fd, reinterpret_cast<const char*>(data), static_cast<int>(len), 0,
             reinterpret_cast<const sockaddr*>(&dst), sizeof(dst));
}

bool PollReadable(int fd, int timeout_ms) {
#ifdef _WIN32
    WSAPOLLFD pfd{};
    pfd.fd = fd;
    pfd.events = POLLRDNORM;
    return WSAPoll(&pfd, 1, timeout_ms) > 0;
#else
    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN;
    return ::poll(&pfd, 1, timeout_ms) > 0;
#endif
}

std::string ExtractId(const char (&raw)[12]) {
    size_t n = 0;
    while (n < 12 && raw[n] != '\0') ++n;
    return std::string(raw, n);
}

} // namespace

LiteServer::LiteServer(const ServerConfig& cfg) : cfg_(cfg) {}

LiteServer::~LiteServer() {
    Stop();
}

bool LiteServer::Start() {
    if (running_.load()) return true;

    hbbs_sock_ = CreateBoundUdpSocket(cfg_.rendezvous_port, bound_hbbs_port_);
    if (hbbs_sock_ < 0) return false;

    hbbr_sock_ = CreateBoundUdpSocket(cfg_.relay_port, bound_hbbr_port_);
    if (hbbr_sock_ < 0) {
        CloseSock(hbbs_sock_);
        hbbs_sock_ = -1;
        return false;
    }

    running_.store(true);
    hbbs_thread_ = std::thread(&LiteServer::RunRendezvousLoop, this);
    hbbr_thread_ = std::thread(&LiteServer::RunRelayLoop, this);
    return true;
}

void LiteServer::Stop() {
    if (!running_.exchange(false)) return;
    if (hbbs_thread_.joinable()) hbbs_thread_.join();
    if (hbbr_thread_.joinable()) hbbr_thread_.join();
    CloseSock(hbbs_sock_);
    CloseSock(hbbr_sock_);
    hbbs_sock_ = -1;
    hbbr_sock_ = -1;
}

ServerStats LiteServer::GetStats() const {
    ServerStats s{};
    {
        std::lock_guard<std::mutex> lk(mu_);
        s.registered_peers = peers_.size();
        s.active_relay_sessions = relay_sessions_.size();
    }
    s.punch_requests = punch_requests_.load();
    s.relayed_packets = relayed_packets_.load();
    s.relayed_bytes = relayed_bytes_.load();
    return s;
}

void LiteServer::AuthorizeRelayToken(uint64_t token) {
    std::lock_guard<std::mutex> lk(mu_);
    RelaySession sess{};
    sess.token = token;
    sess.last_active_ms = NowMs();
    relay_sessions_[token] = sess;
}

void LiteServer::CleanupExpired(uint64_t now_ms) {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto it = peers_.begin(); it != peers_.end();) {
        if (now_ms - it->second.last_seen_ms > cfg_.peer_ttl_ms) {
            it = peers_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = relay_sessions_.begin(); it != relay_sessions_.end();) {
        if (now_ms - it->second.last_active_ms > cfg_.relay_ttl_ms) {
            it = relay_sessions_.erase(it);
        } else {
            ++it;
        }
    }
}

void LiteServer::RunRendezvousLoop() {
    alignas(8) uint8_t rx_buf[2048];
    std::mt19937_64 rng(std::random_device{}());
    uint64_t last_gc_ms = NowMs();

    while (running_.load(std::memory_order_relaxed)) {
        if (!PollReadable(hbbs_sock_, 20)) {
            uint64_t now = NowMs();
            if (now - last_gc_ms > 5000) {
                CleanupExpired(now);
                last_gc_ms = now;
            }
            continue;
        }

        while (running_.load(std::memory_order_relaxed)) {
            sockaddr_in src{};
            socklen_t slen = sizeof(src);
            int n = ::recvfrom(hbbs_sock_, reinterpret_cast<char*>(rx_buf), sizeof(rx_buf), 0,
                               reinterpret_cast<sockaddr*>(&src), &slen);
            if (n <= 0) break;
            if (static_cast<size_t>(n) < sizeof(ServerHeader)) continue;

            const auto* hdr = reinterpret_cast<const ServerHeader*>(rx_buf);
            if (hdr->magic != kServerMagic) continue;

            EndpointV4 sender_ep{src.sin_addr.s_addr, src.sin_port};
            uint64_t now = NowMs();

            if (hdr->type == SigMsgType::kRegisterPeer && static_cast<size_t>(n) >= sizeof(MsgRegisterPeer)) {
                const auto* req = reinterpret_cast<const MsgRegisterPeer*>(rx_buf);
                std::string id = ExtractId(req->peer_id);
                if (id.empty()) continue;

                {
                    std::lock_guard<std::mutex> lk(mu_);
                    PeerRecord& rec = peers_[id];
                    rec.id = id;
                    std::memcpy(rec.pubkey, req->pubkey, 32);
                    rec.wan_ep = sender_ep;
                    rec.lan_ep = req->lan_ep;
                    rec.capabilities = req->capabilities;
                    rec.last_seen_ms = now;
                }

                MsgRegisterAck ack{};
                ack.hdr = {kServerMagic, SigMsgType::kRegisterAck, 0, sizeof(MsgRegisterAck) - sizeof(ServerHeader)};
                ack.observed_wan_ep = sender_ep;
                ack.server_time_ms = now;
                SendUdpTo(hbbs_sock_, sender_ep, &ack, sizeof(ack));
            } else if (hdr->type == SigMsgType::kHeartbeat && static_cast<size_t>(n) >= sizeof(MsgHeartbeat)) {
                const auto* hb = reinterpret_cast<const MsgHeartbeat*>(rx_buf);
                std::string id = ExtractId(hb->peer_id);
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    auto it = peers_.find(id);
                    if (it != peers_.end()) {
                        it->second.wan_ep = sender_ep;
                        it->second.capabilities = hb->capabilities;
                        it->second.last_seen_ms = now;
                    }
                }
                ServerHeader ack{kServerMagic, SigMsgType::kHeartbeatAck, 0, 0};
                SendUdpTo(hbbs_sock_, sender_ep, &ack, sizeof(ack));
            } else if (hdr->type == SigMsgType::kPunchRequest && static_cast<size_t>(n) >= sizeof(MsgPunchRequest)) {
                const auto* req = reinterpret_cast<const MsgPunchRequest*>(rx_buf);
                punch_requests_.fetch_add(1, std::memory_order_relaxed);
                std::string target_id = ExtractId(req->target_id);
                std::string client_id = ExtractId(req->client_id);

                PeerRecord host_rec{};
                bool found = false;
                uint64_t token = rng();
                if (token == 0) token = 1;

                {
                    std::lock_guard<std::mutex> lk(mu_);
                    auto it = peers_.find(target_id);
                    if (it != peers_.end() && (now - it->second.last_seen_ms <= cfg_.peer_ttl_ms)) {
                        host_rec = it->second;
                        found = true;
                        RelaySession rs{};
                        rs.token = token;
                        rs.last_active_ms = now;
                        relay_sessions_[token] = rs;
                    }
                }

                if (!found) {
                    ServerHeader nack{kServerMagic, SigMsgType::kPeerOffline, 0, 0};
                    SendUdpTo(hbbs_sock_, sender_ep, &nack, sizeof(nack));
                    continue;
                }

                // Simultaneous UDP Hole-Punch Order to BOTH Client and Host + Pre-authorized Relay Token
                MsgPunchOrder to_client{};
                to_client.hdr = {kServerMagic, SigMsgType::kPunchOrder, 0, sizeof(MsgPunchOrder) - sizeof(ServerHeader)};
                CopyPeerId(to_client.peer_id, host_rec.id.c_str());
                std::memcpy(to_client.peer_pubkey, host_rec.pubkey, 32);
                to_client.peer_wan_ep = host_rec.wan_ep;
                to_client.peer_lan_ep = host_rec.lan_ep;
                to_client.relay_port_be = htons(bound_hbbr_port_);
                to_client.relay_token = token;
                to_client.is_initiator = 1;

                MsgPunchOrder to_host{};
                to_host.hdr = {kServerMagic, SigMsgType::kPunchOrder, 0, sizeof(MsgPunchOrder) - sizeof(ServerHeader)};
                CopyPeerId(to_host.peer_id, client_id.c_str());
                std::memcpy(to_host.peer_pubkey, req->client_pubkey, 32);
                to_host.peer_wan_ep = sender_ep;
                to_host.peer_lan_ep = req->client_lan_ep;
                to_host.relay_port_be = htons(bound_hbbr_port_);
                to_host.relay_token = token;
                to_host.is_initiator = 0;

                SendUdpTo(hbbs_sock_, sender_ep, &to_client, sizeof(to_client));
                SendUdpTo(hbbs_sock_, host_rec.wan_ep, &to_host, sizeof(to_host));
            }
        }
    }
}

void LiteServer::RunRelayLoop() {
    // Fixed stack/preallocated zero-allocation forwarding buffer (MTU safe up to 64KB)
    alignas(64) uint8_t packet_buf[65536];

    while (running_.load(std::memory_order_relaxed)) {
        if (!PollReadable(hbbr_sock_, 20)) continue;

        while (running_.load(std::memory_order_relaxed)) {
            sockaddr_in src{};
            socklen_t slen = sizeof(src);
            int n = ::recvfrom(hbbr_sock_, reinterpret_cast<char*>(packet_buf), sizeof(packet_buf), 0,
                               reinterpret_cast<sockaddr*>(&src), &slen);
            if (n <= 0) break;
            if (static_cast<size_t>(n) < sizeof(ServerHeader)) continue;

            const auto* hdr = reinterpret_cast<const ServerHeader*>(packet_buf);
            if (hdr->magic != kServerMagic) continue;

            EndpointV4 sender_ep{src.sin_addr.s_addr, src.sin_port};
            uint64_t now = NowMs();

            if (hdr->type == SigMsgType::kRelayBind && static_cast<size_t>(n) >= sizeof(MsgRelayBind)) {
                const auto* bind_msg = reinterpret_cast<const MsgRelayBind*>(packet_buf);
                bool ready = false;
                EndpointV4 peer_a{}, peer_b{};

                {
                    std::lock_guard<std::mutex> lk(mu_);
                    auto it = relay_sessions_.find(bind_msg->relay_token);
                    if (it != relay_sessions_.end()) {
                        it->second.last_active_ms = now;
                        if (bind_msg->role == 0) {
                            it->second.host_ep = sender_ep;
                            it->second.has_host = true;
                        } else {
                            it->second.client_ep = sender_ep;
                            it->second.has_client = true;
                        }
                        ready = (it->second.has_host && it->second.has_client);
                        peer_a = it->second.host_ep;
                        peer_b = it->second.client_ep;
                    }
                }

                ServerHeader ack{kServerMagic, SigMsgType::kRelayBindAck, ready ? uint8_t(1) : uint8_t(0), 0};
                SendUdpTo(hbbr_sock_, sender_ep, &ack, sizeof(ack));
                if (ready) {
                    SendUdpTo(hbbr_sock_, peer_a, &ack, sizeof(ack));
                    SendUdpTo(hbbr_sock_, peer_b, &ack, sizeof(ack));
                }
            } else if (hdr->type == SigMsgType::kRelayData && static_cast<size_t>(n) > sizeof(RelayFrameHeader)) {
                const auto* rhdr = reinterpret_cast<const RelayFrameHeader*>(packet_buf);
                EndpointV4 target_ep{};
                bool can_forward = false;

                {
                    std::lock_guard<std::mutex> lk(mu_);
                    auto it = relay_sessions_.find(rhdr->relay_token);
                    if (it != relay_sessions_.end() && it->second.has_host && it->second.has_client) {
                        it->second.last_active_ms = now;
                        it->second.bytes_forwarded += static_cast<uint64_t>(n);
                        if (sender_ep.ip_be == it->second.client_ep.ip_be &&
                            sender_ep.port_be == it->second.client_ep.port_be) {
                            target_ep = it->second.host_ep;
                            can_forward = true;
                        } else if (sender_ep.ip_be == it->second.host_ep.ip_be &&
                                   sender_ep.port_be == it->second.host_ep.port_be) {
                            target_ep = it->second.client_ep;
                            can_forward = true;
                        }
                    }
                }

                // Zero-copy slice forward: strip 16B RelayFrameHeader in-place by advancing pointer!
                if (can_forward) {
                    const uint8_t* inner_payload = packet_buf + sizeof(RelayFrameHeader);
                    size_t inner_len = static_cast<size_t>(n) - sizeof(RelayFrameHeader);
                    SendUdpTo(hbbr_sock_, target_ep, inner_payload, inner_len);
                    relayed_packets_.fetch_add(1, std::memory_order_relaxed);
                    relayed_bytes_.fetch_add(inner_len, std::memory_order_relaxed);
                }
            }
        }
    }
}

} // namespace uu::server
