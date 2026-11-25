#include "session_engine.hpp"

#include <algorithm>
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
using socklen_t = int;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace uu::core {

namespace {

uint64_t NowMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

uint64_t NowUs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

void CloseSocketCross(int fd) {
    if (fd < 0) return;
#ifdef _WIN32
    closesocket(fd);
#else
    close(fd);
#endif
}

int OpenNonBlockingUdp(uint16_t& out_port) {
#ifdef _WIN32
    static bool wsa_ok = []() {
        WSADATA wsa;
        return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    }();
    (void)wsa_ok;
#endif
    int fd = static_cast<int>(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    if (fd < 0) return -1;

    int buf_sz = 4 * 1024 * 1024;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&buf_sz), sizeof(buf_sz));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&buf_sz), sizeof(buf_sz));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = 0;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        CloseSocketCross(fd);
        return -1;
    }

    sockaddr_in bound{};
    socklen_t blen = sizeof(bound);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &blen) == 0) {
        out_port = ntohs(bound.sin_port);
    }

#ifdef _WIN32
    u_long nb = 1;
    ioctlsocket(fd, FIONBIO, &nb);
#else
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
    return fd;
}

void SendUdp(int fd, const uu::server::EndpointV4& ep, const void* buf, size_t len) {
    if (fd < 0 || ep.port_be == 0) return;
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = ep.ip_be;
    dst.sin_port = ep.port_be;
    ::sendto(fd, reinterpret_cast<const char*>(buf), static_cast<int>(len), 0,
             reinterpret_cast<const sockaddr*>(&dst), sizeof(dst));
}

bool PollUdp(int fd, int timeout_ms) {
#ifdef _WIN32
    WSAPOLLFD pfd{static_cast<SOCKET>(fd), POLLRDNORM, 0};
    return WSAPoll(&pfd, 1, timeout_ms) > 0;
#else
    pollfd pfd{fd, POLLIN, 0};
    return ::poll(&pfd, 1, timeout_ms) > 0;
#endif
}

uint32_t DetectLocalRouteIpBe(const uu::server::EndpointV4& target_ep) {
    int probe = static_cast<int>(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    if (probe >= 0) {
        sockaddr_in dst{};
        dst.sin_family = AF_INET;
        dst.sin_addr.s_addr = target_ep.ip_be;
        dst.sin_port = target_ep.port_be ? target_ep.port_be : htons(80);
        if (::connect(probe, reinterpret_cast<const sockaddr*>(&dst), sizeof(dst)) == 0) {
            sockaddr_in loc{};
            socklen_t len = sizeof(loc);
            if (::getsockname(probe, reinterpret_cast<sockaddr*>(&loc), &len) == 0 &&
                loc.sin_addr.s_addr != 0) {
                CloseSocketCross(probe);
                return loc.sin_addr.s_addr;
            }
        }
        CloseSocketCross(probe);
    }
    in_addr loop_ip{};
    inet_pton(AF_INET, "127.0.0.1", &loop_ip);
    return loop_ip.s_addr;
}

#pragma pack(push, 1)
struct HandshakeHello {
    uint8_t static_pub[32];
    uint64_t send_ts_us;
    uint32_t codec_caps; // Bit 0: TileDeltaLz4, Bit 1: Apple HEVC, Bit 2: Hardware JPEG (Flutter libjpeg-turbo)
};
#pragma pack(pop)

uint32_t LocalCodecCaps() {
#if defined(__APPLE__)
    return 0x07;
#else
    return 0x05;
#endif
}

} // namespace

PeerNode::PeerNode(const NodeConfig& cfg)
    : cfg_(cfg), static_keys_(CryptoEngine::GenerateKeyPair()) {
    in_addr ip{};
    inet_pton(AF_INET, cfg_.server_ip.c_str(), &ip);
    hbbs_ep_.ip_be = ip.s_addr;
    hbbs_ep_.port_be = htons(cfg_.hbbs_port);
    hbbr_ep_.ip_be = ip.s_addr;
    hbbr_ep_.port_be = htons(cfg_.hbbr_port);

    if (cfg_.is_host) {
        if (cfg_.use_synthetic_capture) {
            auto syn = std::make_unique<SyntheticCapturer>(1280, 720);
            synthetic_ptr_ = syn.get();
            capturer_ = std::move(syn);
        } else {
            capturer_ = CreateNativeCapturer();
            input_dispatcher_.SetOsInjectionEnabled(true);
        }
        encoder_ = CreateHybridVideoEncoder();
    } else {
        decoder_ = CreateHybridVideoDecoder();
    }
}

PeerNode::~PeerNode() {
    Stop();
}

bool PeerNode::Start() {
    if (running_.load()) return true;
    sock_ = OpenNonBlockingUdp(local_port_);
    if (sock_ < 0) return false;

    window_start_ms_ = NowMs();
    running_.store(true);

    // Register with hbbs rendezvous immediately
    uu::server::MsgRegisterPeer reg{};
    reg.hdr = {uu::server::kServerMagic, uu::server::SigMsgType::kRegisterPeer, 0,
               sizeof(reg) - sizeof(uu::server::ServerHeader)};
    uu::server::CopyPeerId(reg.peer_id, cfg_.self_id.c_str());
    std::memcpy(reg.pubkey, static_keys_.pub.data(), 32);
    reg.lan_ep = {DetectLocalRouteIpBe(hbbs_ep_), htons(local_port_)};
    reg.capabilities = 0x07;
    SendUdp(sock_, hbbs_ep_, &reg, sizeof(reg));

    net_thread_ = std::thread(&PeerNode::NetworkLoop, this);
    if (cfg_.is_host) {
        capture_thread_ = std::thread(&PeerNode::HostCaptureLoop, this);
    }
    return true;
}

void PeerNode::Stop() {
    if (!running_.exchange(false)) return;
    input_dispatcher_.ReleaseAllHeldKeys();
    if (capture_thread_.joinable()) capture_thread_.join();
    if (net_thread_.joinable()) net_thread_.join();
    CloseSocketCross(sock_);
    sock_ = -1;
}

bool PeerNode::ConnectToPeer(const std::string& target_host_id, uint32_t timeout_ms) {
    punch_started_ms_ = NowMs();

    uu::server::MsgPunchRequest req{};
    req.hdr = {uu::server::kServerMagic, uu::server::SigMsgType::kPunchRequest, 0,
               sizeof(req) - sizeof(uu::server::ServerHeader)};
    uu::server::CopyPeerId(req.client_id, cfg_.self_id.c_str());
    uu::server::CopyPeerId(req.target_id, target_host_id.c_str());
    std::memcpy(req.client_pubkey, static_keys_.pub.data(), 32);
    req.client_lan_ep = {DetectLocalRouteIpBe(hbbs_ep_), htons(local_port_)};

    SendUdp(sock_, hbbs_ep_, &req, sizeof(req));

    uint64_t deadline = NowMs() + timeout_ms;
    while (NowMs() < deadline) {
        if (IsConnected()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return IsConnected();
}

SessionTelemetry PeerNode::GetTelemetry() const {
    std::lock_guard<std::mutex> lk(state_mu_);
    SessionTelemetry copy = telemetry_;
    copy.path = path_.load();
    return copy;
}

void PeerNode::SetSyntheticWorkload(SyntheticWorkload mode) {
    if (synthetic_ptr_) {
        synthetic_ptr_->SetWorkload(mode);
    }
}

void PeerNode::SendCursorMoveFast(uint16_t x_norm, uint16_t y_norm, int16_t dx, int16_t dy,
                                  uint8_t button_mask, int16_t wheel_y) {
    PayloadMouseEvent ev{};
    ev.x_norm = x_norm;
    ev.y_norm = y_norm;
    ev.dx = dx;
    ev.dy = dy;
    ev.wheel_y = wheel_y;
    ev.button_mask = button_mask;
    ev.is_relative = (dx != 0 || dy != 0) ? 1 : 0;
    if (button_mask != prev_tx_btn_mask_) {
        prev_tx_btn_mask_ = button_mask;
        uint32_t rseq = p2_seq_.fetch_add(1);
        SendEncrypted(QoSLane::kP2_Reliable, PacketType::kMouseClick, &ev, sizeof(ev), rseq);
    } else {
        SendEncrypted(QoSLane::kP0_FastInput, PacketType::kMouseFast, &ev, sizeof(ev));
    }
}

void PeerNode::SendKey(uint16_t keycode, bool is_down, uint8_t modifiers) {
    PayloadKeyInput ev{keycode, static_cast<uint8_t>(is_down ? 1 : 0), modifiers};
    uint32_t rseq = p2_seq_.fetch_add(1);
    SendEncrypted(QoSLane::kP2_Reliable, PacketType::kKeyInput, &ev, sizeof(ev), rseq);
}

void PeerNode::SendGamepad(const PayloadGamepad& pad) {
    SendEncrypted(QoSLane::kP0_FastInput, PacketType::kGamepadState, &pad, sizeof(pad));
}

void PeerNode::SetQualityProfile(QualityProfile profile, uint8_t target_fps,
                                 bool privacy_curtain, uint8_t monitor_index) {
    bwe_.SetProfile(profile);
    PayloadQualityControl qc{};
    qc.profile = profile;
    qc.target_fps = target_fps;
    qc.monitor_index = monitor_index;
    qc.privacy_curtain = privacy_curtain ? 1 : 0;
    qc.max_bitrate_kbps = (profile == QualityProfile::kUltraSaver300K) ? 600 : 25000;
    uint32_t rseq = p2_seq_.fetch_add(1);
    SendEncrypted(QoSLane::kP2_Reliable, PacketType::kQualityControl, &qc, sizeof(qc), rseq);
}

void PeerNode::SendClipboardText(const std::string& utf8_text) {
    auto packed = ResumableFileEngine::PackClipboardText(utf8_text);
    uint32_t rseq = p2_seq_.fetch_add(1);
    SendEncrypted(QoSLane::kP2_Reliable, PacketType::kClipboardSync, packed.data(), packed.size(), rseq);
}

bool PeerNode::SendFile(const std::string& local_file_path, uint64_t file_id) {
    PayloadFileOffer offer{};
    if (!file_engine_.PrepareSendFromFile(local_file_path, file_id, offer)) return false;

    uint32_t rseq = p2_seq_.fetch_add(1);
    SendEncrypted(QoSLane::kP2_Reliable, PacketType::kFileOffer, &offer, sizeof(offer), rseq);

    uint64_t offset = 0;
    std::vector<uint8_t> chunk_pkt;
    while (file_engine_.ReadSendChunk(file_id, offset, chunk_pkt)) {
        uint32_t cseq = p2_seq_.fetch_add(1);
        SendEncrypted(QoSLane::kP2_Reliable, PacketType::kFileChunk,
                      chunk_pkt.data(), chunk_pkt.size(), cseq);
        auto* chdr = reinterpret_cast<const PayloadFileChunkHeader*>(chunk_pkt.data());
        offset += chdr->chunk_len;
    }
    return true;
}

std::string PeerNode::LastClipboardText() const {
    std::lock_guard<std::mutex> lk(state_mu_);
    return last_clipboard_;
}

bool PeerNode::CopyLatestFrameBgra(std::vector<uint8_t>& out_bgra, uint16_t& out_w, uint16_t& out_h) const {
    std::lock_guard<std::mutex> lk(state_mu_);
    if (latest_bgra_.empty()) return false;
    out_bgra = latest_bgra_;
    out_w = latest_w_;
    out_h = latest_h_;
    return true;
}

bool PeerNode::CopyLatestFrameEx(uint64_t last_seen_seq,
                                 uint64_t& out_seq,
                                 uint8_t& out_format,
                                 std::vector<uint8_t>& out_bytes,
                                 uint16_t& out_w,
                                 uint16_t& out_h) const {
    std::lock_guard<std::mutex> lk(state_mu_);
    if (latest_frame_seq_ == 0 || latest_frame_seq_ == last_seen_seq) return false;
    out_seq = latest_frame_seq_;
    out_format = latest_frame_format_;
    out_w = latest_w_;
    out_h = latest_h_;
    if (latest_frame_format_ == 2) {
        if (latest_jpeg_.empty()) return false;
        out_bytes = latest_jpeg_;
        return true;
    }
    if (latest_bgra_.empty()) return false;
    out_bytes = latest_bgra_;
    return true;
}

void PeerNode::SendEncrypted(QoSLane lane, PacketType type, const void* payload, size_t len, uint32_t ack_or_group) {
    if (!has_session_key_) return;

    uint32_t seq = tx_seq_.fetch_add(1);
    WireHeader hdr{};
    hdr.magic = kWireMagic;
    hdr.type = type;
    hdr.lane = lane;
    hdr.payload_len = static_cast<uint16_t>(len);
    hdr.seq = seq;
    hdr.ack_or_group = ack_or_group;

    std::vector<uint8_t> ct_tag;
    uint64_t nonce64 = (static_cast<uint64_t>(cfg_.is_host ? 1 : 2) << 48) | seq;
    CryptoEngine::SealPacket(session_key_, nonce64,
                             reinterpret_cast<const uint8_t*>(&hdr), sizeof(hdr),
                             reinterpret_cast<const uint8_t*>(payload), len, ct_tag);

    std::vector<uint8_t> wire(sizeof(hdr) + ct_tag.size());
    std::memcpy(wire.data(), &hdr, sizeof(hdr));
    std::memcpy(wire.data() + sizeof(hdr), ct_tag.data(), ct_tag.size());

    uint32_t reliable_track = (lane == QoSLane::kP2_Reliable && type != PacketType::kAck) ? ack_or_group : 0;
    scheduler_.Enqueue(lane, type, std::move(wire), reliable_track);
}

void PeerNode::SendRawWireToActivePath(const uint8_t* data, size_t len) {
    TransportPath cur_path = path_.load();
    if (cur_path == TransportPath::kP2pDirect && !cfg_.block_direct_p2p_for_test) {
        SendUdp(sock_, peer_wan_ep_, data, len);
    } else if (relay_token_ != 0) {
        std::vector<uint8_t> wrapped(sizeof(uu::server::RelayFrameHeader) + len);
        uu::server::RelayFrameHeader rhdr{};
        rhdr.hdr = {uu::server::kServerMagic, uu::server::SigMsgType::kRelayData, 0,
                    static_cast<uint16_t>(len)};
        rhdr.relay_token = relay_token_;
        std::memcpy(wrapped.data(), &rhdr, sizeof(rhdr));
        std::memcpy(wrapped.data() + sizeof(rhdr), data, len);
        SendUdp(sock_, hbbr_ep_, wrapped.data(), wrapped.size());
    }
}

void PeerNode::FlushScheduler() {
    std::vector<uint8_t> pkt;
    int burst_guard = 0;
    while (burst_guard++ < 24 && scheduler_.PopNext(pkt)) {
        bytes_window_ += pkt.size();
        SendRawWireToActivePath(pkt.data(), pkt.size());
    }
}

void PeerNode::NetworkLoop() {
    alignas(8) uint8_t rx_buf[65536];
    std::mt19937 drop_rng(42);
    std::uniform_real_distribution<float> dist(0.0f, 100.0f);
    uint64_t last_ping_ms = NowMs();
    uint64_t last_hb_ms = NowMs();

    while (running_.load(std::memory_order_relaxed)) {
        FlushScheduler();
        PollUdp(sock_, 1);

        while (running_.load(std::memory_order_relaxed)) {
            sockaddr_in src{};
            socklen_t slen = sizeof(src);
            int n = ::recvfrom(sock_, reinterpret_cast<char*>(rx_buf), sizeof(rx_buf), 0,
                               reinterpret_cast<sockaddr*>(&src), &slen);
            if (n <= 0) break;

            uu::server::EndpointV4 sender{src.sin_addr.s_addr, src.sin_port};
            bool from_relay = (sender.port_be == hbbr_ep_.port_be);

            // Check if this is a Server Control message (hbbs / hbbr)
            if (static_cast<size_t>(n) >= sizeof(uu::server::ServerHeader)) {
                const auto* shdr = reinterpret_cast<const uu::server::ServerHeader*>(rx_buf);
                if (shdr->magic == uu::server::kServerMagic) {
                    if (shdr->type == uu::server::SigMsgType::kPunchOrder &&
                        static_cast<size_t>(n) >= sizeof(uu::server::MsgPunchOrder)) {
                        const auto* ord = reinterpret_cast<const uu::server::MsgPunchOrder*>(rx_buf);
                        peer_wan_ep_ = ord->peer_wan_ep;
                        peer_lan_ep_ = ord->peer_lan_ep;
                        hbbr_ep_.port_be = ord->relay_port_be;
                        relay_token_ = ord->relay_token;

                        // Reset session queues cleanly so repeat client reconnections always succeed
                        path_.store(TransportPath::kDisconnected);
                        scheduler_.ClearMediaQueue();
                        last_rx_seq_ = 0;
                        if (encoder_) {
                            encoder_->SetPeerSupportsHevc(false);
                        }

                        std::array<uint8_t, 32> peer_pub{};
                        std::memcpy(peer_pub.data(), ord->peer_pubkey, 32);
                        auto shared = CryptoEngine::X25519SharedSecret(static_keys_.secret, peer_pub);
                        session_key_ = CryptoEngine::DeriveSessionKey(shared, cfg_.access_pin.c_str());
                        has_session_key_ = true;
                        if (punch_started_ms_ == 0) punch_started_ms_ = NowMs();

                        // Happy Eyeballs:
                        // 1) Immediately fire UDP P2P hole-punch Hello directly to peer WAN & LAN
                        // 2) Simultaneously pre-bind relay token on hbbr so Symmetric NAT has 0-wait fallback!
                        uu::server::MsgRelayBind rbind{};
                        rbind.hdr = {uu::server::kServerMagic, uu::server::SigMsgType::kRelayBind, 0,
                                     sizeof(rbind) - sizeof(uu::server::ServerHeader)};
                        rbind.relay_token = relay_token_;
                        rbind.role = cfg_.is_host ? 0 : 1;
                        SendUdp(sock_, hbbr_ep_, &rbind, sizeof(rbind));

                        HandshakeHello hello{};
                        std::memcpy(hello.static_pub, static_keys_.pub.data(), 32);
                        hello.send_ts_us = NowUs();
                        hello.codec_caps = LocalCodecCaps();
                        SendEncrypted(QoSLane::kP0_FastInput, PacketType::kHelloInit, &hello, sizeof(hello));

                        std::vector<uint8_t> hello_wire;
                        if (scheduler_.PopNext(hello_wire)) {
                            if (!cfg_.block_direct_p2p_for_test) {
                                SendUdp(sock_, peer_wan_ep_, hello_wire.data(), hello_wire.size());
                                if (peer_lan_ep_.port_be != 0) {
                                    SendUdp(sock_, peer_lan_ep_, hello_wire.data(), hello_wire.size());
                                }
                            }
                            // Also send via relay if direct P2P is blocked or after fast race
                            uu::server::RelayFrameHeader rhdr{};
                            rhdr.hdr = {uu::server::kServerMagic, uu::server::SigMsgType::kRelayData, 0,
                                        static_cast<uint16_t>(hello_wire.size())};
                            rhdr.relay_token = relay_token_;
                            std::vector<uint8_t> rwrap(sizeof(rhdr) + hello_wire.size());
                            std::memcpy(rwrap.data(), &rhdr, sizeof(rhdr));
                            std::memcpy(rwrap.data() + sizeof(rhdr), hello_wire.data(), hello_wire.size());
                            SendUdp(sock_, hbbr_ep_, rwrap.data(), rwrap.size());
                        }
                    } else if (shdr->type == uu::server::SigMsgType::kRelayBindAck) {
                        relay_bound_ = (shdr->flags == 1);
                        if (relay_bound_ && path_.load() == TransportPath::kDisconnected && has_session_key_) {
                            // Relay paired! Fire Hello over relay immediately
                            HandshakeHello hello{};
                            std::memcpy(hello.static_pub, static_keys_.pub.data(), 32);
                            hello.send_ts_us = NowUs();
                            hello.codec_caps = LocalCodecCaps();
                            SendEncrypted(QoSLane::kP0_FastInput, PacketType::kHelloInit, &hello, sizeof(hello));
                        }
                    }
                    continue;
                }
            }

            // Optional weak-network packet drop simulator (for video/media packets)
            if (cfg_.simulated_rx_loss_pct > 0.0f && static_cast<size_t>(n) >= sizeof(WireHeader)) {
                const auto* wh = reinterpret_cast<const WireHeader*>(rx_buf);
                if (wh->magic == kWireMagic &&
                    (wh->type == PacketType::kVideoShard || wh->type == PacketType::kVideoFecParity)) {
                    if (dist(drop_rng) < cfg_.simulated_rx_loss_pct) {
                        ++lost_pkt_window_;
                        continue; // Simulated UDP drop!
                    }
                }
            }

            if (cfg_.block_direct_p2p_for_test && !from_relay) {
                continue; // Simulate Symmetric NAT blocking direct P2P packets
            }

            HandleWirePacket(rx_buf, static_cast<size_t>(n), sender, from_relay);
        }

        uint64_t now = NowMs();
        scheduler_.PumpRetransmissions(now, 60);

        if (IsConnected() && now - last_ping_ms >= 100) {
            last_ping_ms = now;
            PayloadPingPong pp{};
            pp.send_ts_us = NowUs();
            pp.rx_packets_window = rx_pkt_window_;
            pp.lost_packets_window = lost_pkt_window_;
            pp.jitter_us = 500;
            SendEncrypted(QoSLane::kP0_FastInput, PacketType::kPing, &pp, sizeof(pp));
        }

        if (now - window_start_ms_ >= 250) {
            double elapsed_sec = static_cast<double>(now - window_start_ms_) / 1000.0;
            std::lock_guard<std::mutex> lk(state_mu_);
            telemetry_.realtime_kbps = static_cast<float>((bytes_window_ * 8.0 / 1000.0) / elapsed_sec);
            telemetry_.current_fps = static_cast<float>(frames_window_ / elapsed_sec);
            bytes_window_ = 0;
            frames_window_ = 0;
            window_start_ms_ = now;
        }

        if (now - last_hb_ms >= 20000) {
            last_hb_ms = now;
            uu::server::MsgHeartbeat hb{};
            hb.hdr = {uu::server::kServerMagic, uu::server::SigMsgType::kHeartbeat, 0,
                      sizeof(hb) - sizeof(uu::server::ServerHeader)};
            uu::server::CopyPeerId(hb.peer_id, cfg_.self_id.c_str());
            hb.timestamp_ms = now;
            hb.capabilities = 0x07;
            SendUdp(sock_, hbbs_ep_, &hb, sizeof(hb));
        }
    }
}

void PeerNode::HandleWirePacket(const uint8_t* raw, size_t len,
                                const uu::server::EndpointV4& sender_ep, bool via_relay) {
    if (!has_session_key_ || len < sizeof(WireHeader) + 16) return;
    const auto* hdr = reinterpret_cast<const WireHeader*>(raw);
    if (hdr->magic != kWireMagic) return;
    if (sizeof(WireHeader) + hdr->payload_len + 16 != len) return;

    uint64_t peer_role = cfg_.is_host ? 2 : 1; // Opposite role sealed this packet
    uint64_t nonce64 = (peer_role << 48) | hdr->seq;

    std::vector<uint8_t> pt;
    if (!CryptoEngine::OpenPacket(session_key_, nonce64, raw, sizeof(WireHeader),
                                  raw + sizeof(WireHeader), hdr->payload_len + 16, pt)) {
        return; // Tampered or wrong access PIN!
    }

    bytes_window_ += len;
    ++rx_pkt_window_;
    if (last_rx_seq_ > 0 && hdr->seq > last_rx_seq_ + 1) {
        lost_pkt_window_ += (hdr->seq - last_rx_seq_ - 1);
    }
    if (hdr->seq > last_rx_seq_) last_rx_seq_ = hdr->seq;

    // Upgrade to P2P Direct immediately if a direct packet arrives!
    if (!via_relay) {
        peer_wan_ep_ = sender_ep;
        if (path_.load() != TransportPath::kP2pDirect) {
            path_.store(TransportPath::kP2pDirect);
            std::lock_guard<std::mutex> lk(state_mu_);
            if (telemetry_.connect_time_ms == 0.0f && punch_started_ms_ > 0) {
                telemetry_.connect_time_ms = static_cast<float>(NowMs() - punch_started_ms_);
            }
        }
    } else if (path_.load() == TransportPath::kDisconnected) {
        path_.store(TransportPath::kRelayFallback);
        std::lock_guard<std::mutex> lk(state_mu_);
        if (telemetry_.connect_time_ms == 0.0f && punch_started_ms_ > 0) {
            telemetry_.connect_time_ms = static_cast<float>(NowMs() - punch_started_ms_);
        }
    }

    // Send immediate ACK for P2 reliable packets
    if (hdr->lane == QoSLane::kP2_Reliable && hdr->type != PacketType::kAck && hdr->ack_or_group != 0) {
        uint32_t ack_seq = hdr->ack_or_group;
        SendEncrypted(QoSLane::kP2_Reliable, PacketType::kAck, &ack_seq, sizeof(ack_seq));
    }

    switch (hdr->type) {
        case PacketType::kHelloInit: {
            if (encoder_) {
                bool peer_hevc = false;
                bool peer_jpeg = false;
                if (pt.size() >= sizeof(HandshakeHello)) {
                    HandshakeHello peer_h{};
                    std::memcpy(&peer_h, pt.data(), sizeof(peer_h));
                    peer_hevc = (peer_h.codec_caps & 0x02) != 0;
                    peer_jpeg = (peer_h.codec_caps & 0x04) != 0;
                }
                encoder_->SetPeerSupportsHevc(peer_hevc);
                encoder_->SetPeerSupportsJpeg((!cfg_.use_synthetic_capture || force_jpeg_test_) && peer_jpeg);
            }
            HandshakeHello accept{};
            std::memcpy(accept.static_pub, static_keys_.pub.data(), 32);
            accept.send_ts_us = NowUs();
            accept.codec_caps = LocalCodecCaps();
            SendEncrypted(QoSLane::kP0_FastInput, PacketType::kHelloAccept, &accept, sizeof(accept));
            force_next_idr_.store(true);
            break;
        }
        case PacketType::kHelloAccept: {
            if (encoder_) {
                bool peer_hevc = false;
                bool peer_jpeg = false;
                if (pt.size() >= sizeof(HandshakeHello)) {
                    HandshakeHello peer_h{};
                    std::memcpy(&peer_h, pt.data(), sizeof(peer_h));
                    peer_hevc = (peer_h.codec_caps & 0x02) != 0;
                    peer_jpeg = (peer_h.codec_caps & 0x04) != 0;
                }
                encoder_->SetPeerSupportsHevc(peer_hevc);
                encoder_->SetPeerSupportsJpeg((!cfg_.use_synthetic_capture || force_jpeg_test_) && peer_jpeg);
            }
            force_next_idr_.store(true);
            break;
        }
        case PacketType::kRequestIdr: {
            force_next_idr_.store(true);
            break;
        }
        case PacketType::kAck: {
            if (pt.size() >= 4) {
                uint32_t acked = 0;
                std::memcpy(&acked, pt.data(), 4);
                scheduler_.AckReliable(acked);
            }
            break;
        }
        case PacketType::kPing: {
            if (pt.size() >= sizeof(PayloadPingPong)) {
                SendEncrypted(QoSLane::kP0_FastInput, PacketType::kPong, pt.data(), sizeof(PayloadPingPong));
            }
            break;
        }
        case PacketType::kPong: {
            if (pt.size() >= sizeof(PayloadPingPong)) {
                PayloadPingPong pp{};
                std::memcpy(&pp, pt.data(), sizeof(pp));
                float rtt = static_cast<float>(NowUs() - pp.send_ts_us) / 1000.0f;
                uint32_t total = pp.rx_packets_window + pp.lost_packets_window;
                float loss = total > 0 ? (100.0f * pp.lost_packets_window / static_cast<float>(total)) : 0.0f;
                std::lock_guard<std::mutex> lk(state_mu_);
                telemetry_.rtt_ms = (telemetry_.rtt_ms == 0.0f) ? rtt : (telemetry_.rtt_ms * 0.7f + rtt * 0.3f);
                telemetry_.loss_pct = loss;
            }
            break;
        }
        case PacketType::kCursorPos: {
            if (pt.size() >= sizeof(PayloadCursorPos)) {
                PayloadCursorPos cp{};
                std::memcpy(&cp, pt.data(), sizeof(cp));
                std::lock_guard<std::mutex> lk(state_mu_);
                telemetry_.cursor_x_norm = cp.x;
                telemetry_.cursor_y_norm = cp.y;
                ++telemetry_.cursor_updates;
            }
            break;
        }
        case PacketType::kCursorShape: {
            if (pt.size() >= sizeof(PayloadCursorShapeHeader)) {
                PayloadCursorShapeHeader sh{};
                std::memcpy(&sh, pt.data(), sizeof(sh));
                if (sizeof(sh) + sh.compressed_len <= pt.size()) {
                    size_t raw_sz = static_cast<size_t>(sh.width) * sh.height * 4;
                    std::vector<uint8_t> rgba(raw_sz);
                    if (DecompressFastLz4(pt.data() + sizeof(sh), sh.compressed_len, rgba.data(), raw_sz)) {
                        std::lock_guard<std::mutex> lk(state_mu_);
                        cursor_shape_cache_[sh.shape_hash] = std::move(rgba);
                    }
                }
            }
            break;
        }
        case PacketType::kVideoShard:
        case PacketType::kVideoFecParity: {
            HandleVideoPacket(hdr->type, pt.data(), pt.size());
            break;
        }
        case PacketType::kMouseFast:
        case PacketType::kMouseClick: {
            if (pt.size() >= sizeof(PayloadMouseEvent)) {
                PayloadMouseEvent ev{};
                std::memcpy(&ev, pt.data(), sizeof(ev));
                input_dispatcher_.ApplyMouse(ev);
            }
            break;
        }
        case PacketType::kKeyInput: {
            if (pt.size() >= sizeof(PayloadKeyInput)) {
                PayloadKeyInput ev{};
                std::memcpy(&ev, pt.data(), sizeof(ev));
                input_dispatcher_.ApplyKey(ev);
            }
            break;
        }
        case PacketType::kGamepadState: {
            if (pt.size() >= sizeof(PayloadGamepad)) {
                PayloadGamepad pad{};
                std::memcpy(&pad, pt.data(), sizeof(pad));
                input_dispatcher_.ApplyGamepad(pad);
            }
            break;
        }
        case PacketType::kQualityControl: {
            if (pt.size() >= sizeof(PayloadQualityControl)) {
                PayloadQualityControl qc{};
                std::memcpy(&qc, pt.data(), sizeof(qc));
                bwe_.SetProfile(qc.profile);
                if (capturer_ && qc.monitor_index != telemetry_.active_monitor) {
                    if (capturer_->SelectMonitor(qc.monitor_index)) {
                        scheduler_.ClearMediaQueue();
                    }
                }
                force_next_idr_.store(true);
                std::lock_guard<std::mutex> lk(state_mu_);
                telemetry_.active_profile = qc.profile;
                telemetry_.active_monitor = qc.monitor_index;
                telemetry_.privacy_curtain_on = (qc.privacy_curtain != 0);
            }
            break;
        }
        case PacketType::kClipboardSync: {
            std::string txt = ResumableFileEngine::UnpackClipboardText(pt.data(), pt.size());
            if (!txt.empty()) {
                std::lock_guard<std::mutex> lk(state_mu_);
                last_clipboard_ = std::move(txt);
            }
            break;
        }
        case PacketType::kFileOffer: {
            if (pt.size() >= sizeof(PayloadFileOffer)) {
                PayloadFileOffer offer{};
                std::memcpy(&offer, pt.data(), sizeof(offer));
                file_engine_.AcceptIncomingOffer(offer, cfg_.download_dir);
            }
            break;
        }
        case PacketType::kFileChunk: {
            if (pt.size() >= sizeof(PayloadFileChunkHeader)) {
                PayloadFileChunkHeader ch{};
                std::memcpy(&ch, pt.data(), sizeof(ch));
                if (sizeof(ch) + ch.chunk_len <= pt.size()) {
                    bool done = false;
                    bool sha_ok = false;
                    file_engine_.OnIncomingChunk(ch, pt.data() + sizeof(ch), done, sha_ok);
                    if (done && sha_ok) {
                        last_file_sha256_ok_.store(true);
                    }
                }
            }
            break;
        }
        default:
            break;
    }
}

void PeerNode::HandleVideoPacket(PacketType type, const uint8_t* payload, size_t len) {
    if (!decoder_ || len < sizeof(PayloadVideoShardHeader)) return;

    PayloadVideoShardHeader vhdr{};
    std::memcpy(&vhdr, payload, sizeof(vhdr));
    if (sizeof(vhdr) + vhdr.shard_bytes > len || vhdr.total_data_shards == 0) return;

    auto& assembly = rx_frames_[vhdr.frame_id];
    if (assembly.completed) return;

    if (assembly.total_data == 0) {
        assembly.total_data = vhdr.total_data_shards;
        assembly.parity_cnt = vhdr.parity_shards;
        assembly.width = vhdr.width;
        assembly.height = vhdr.height;
        assembly.codec = vhdr.codec;
        assembly.is_keyframe = (vhdr.is_keyframe != 0);
        assembly.created_ms = NowMs();
        assembly.shards.resize(vhdr.total_data_shards);
    }

    const uint8_t* body = payload + sizeof(vhdr);

    if (type == PacketType::kVideoShard && vhdr.shard_index < assembly.total_data) {
        assembly.shards[vhdr.shard_index] = std::vector<uint8_t>(body, body + vhdr.shard_bytes);
    } else if (type == PacketType::kVideoFecParity) {
        AdaptiveFecCodec::FecParityShard p;
        // dirty_rect_cnt field carries group_count for parity packets; shard_index carries group_start_idx
        p.group_start_idx = vhdr.shard_index;
        p.group_count = vhdr.dirty_rect_cnt;
        p.parity_bytes.assign(body, body + vhdr.shard_bytes);
        assembly.parities.push_back(std::move(p));
    }

    size_t present_before = 0;
    for (const auto& s : assembly.shards) {
        if (s.has_value()) ++present_before;
    }

    // If a newer frame arrives while an older frame was never completed, fire Instant PLI (kRequestIdr)!
    if (vhdr.frame_id > last_decoded_frame_id_ + 1) {
        uint64_t now = NowMs();
        if (now - last_pli_sent_ms_ >= 40) {
            last_pli_sent_ms_ = now;
            uint32_t req_from = last_decoded_frame_id_;
            SendEncrypted(QoSLane::kP0_FastInput, PacketType::kRequestIdr, &req_from, sizeof(req_from));
        }
    }

    bool all_ready = AdaptiveFecCodec::TryReconstructFrame(assembly.total_data, assembly.shards, assembly.parities);
    size_t present_after = 0;
    for (const auto& s : assembly.shards) {
        if (s.has_value()) ++present_after;
    }
    if (present_after > present_before) {
        assembly.used_fec = true;
    }
    if (!all_ready) {
        return; // Still waiting for remaining shards or parity
    }

    assembly.completed = true;
    if (vhdr.frame_id > last_decoded_frame_id_) {
        last_decoded_frame_id_ = vhdr.frame_id;
    }
    EncodedAccessUnit au{};
    au.frame_id = vhdr.frame_id;
    au.width = assembly.width;
    au.height = assembly.height;
    au.codec = assembly.codec;
    au.is_keyframe = assembly.is_keyframe;
    au.payload.reserve(vhdr.total_frame_bytes);
    for (uint16_t i = 0; i < assembly.total_data; ++i) {
        const auto& sh = *assembly.shards[i];
        au.payload.insert(au.payload.end(), sh.begin(), sh.end());
    }

    DecodedFrame df{};
    if (decoder_->Decode(au, df)) {
        ++frames_window_;
        std::lock_guard<std::mutex> lk(state_mu_);
        if (df.codec == VideoCodecId::kJpegFrame && !df.jpeg_bytes.empty()) {
            latest_jpeg_ = std::move(df.jpeg_bytes);
            latest_frame_format_ = 2;
        } else {
            latest_bgra_ = std::move(df.bgra);
            latest_frame_format_ = 1;
        }
        ++latest_frame_seq_;
        latest_w_ = df.width;
        latest_h_ = df.height;
        telemetry_.remote_width = df.width;
        telemetry_.remote_height = df.height;
        telemetry_.active_codec = df.codec;
        if (vhdr.monitor_count > 0) {
            telemetry_.monitor_count = vhdr.monitor_count;
            telemetry_.active_monitor = vhdr.active_monitor;
        }
        ++telemetry_.frames_decoded;
        if (assembly.used_fec) {
            ++telemetry_.fec_recovered_frames;
        }
        float dec_ms = static_cast<float>(df.decode_duration_us) / 1000.0f;
        telemetry_.avg_decode_ms = (telemetry_.avg_decode_ms == 0.0f)
                                       ? dec_ms
                                       : (telemetry_.avg_decode_ms * 0.8f + dec_ms * 0.2f);
    }

    // Garbage-collect old frame assemblies
    for (auto it = rx_frames_.begin(); it != rx_frames_.end();) {
        if (it->first + 16 < vhdr.frame_id) {
            it = rx_frames_.erase(it);
        } else {
            ++it;
        }
    }
}

void PeerNode::HostCaptureLoop() {
    constexpr size_t kMaxShardData = 960;

    while (running_.load(std::memory_order_relaxed)) {
        if (!IsConnected() || !capturer_ || !encoder_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        uint64_t loop_t0 = NowMs();

        // 1. Always capture & transmit decoupled cursor on P0 Fast Lane (12 bytes!)
        CursorSnapshot cur = capturer_->CaptureCursor();
        if (cur.shape_changed && !cur.bgra_pixels.empty()) {
            std::vector<uint8_t> lz4_cur;
            size_t clen = CompressFastLz4(cur.bgra_pixels.data(), cur.bgra_pixels.size(), lz4_cur);
            if (clen > 0) {
                PayloadCursorShapeHeader sh{};
                sh.shape_hash = cur.shape_hash;
                sh.width = cur.width;
                sh.height = cur.height;
                sh.hotspot_x = cur.hotspot_x;
                sh.hotspot_y = cur.hotspot_y;
                sh.compressed_len = static_cast<uint16_t>(clen);
                std::vector<uint8_t> buf(sizeof(sh) + clen);
                std::memcpy(buf.data(), &sh, sizeof(sh));
                std::memcpy(buf.data() + sizeof(sh), lz4_cur.data(), clen);
                SendEncrypted(QoSLane::kP1_Media, PacketType::kCursorShape, buf.data(), buf.size());
            }
        }
        if (cur.x_norm != last_sent_cx_ || cur.y_norm != last_sent_cy_) {
            last_sent_cx_ = cur.x_norm;
            last_sent_cy_ = cur.y_norm;
            PayloadCursorPos cp{cur.x_norm, cur.y_norm, cur.shape_hash};
            SendEncrypted(QoSLane::kP0_FastInput, PacketType::kCursorPos, &cp, sizeof(cp));
        }

        // 2. Backpressure: wait until previous frame's UDP shards finish draining before encoding next frame!
        if (scheduler_.PendingMediaCount() > 16) {
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
            continue;
        }

        // Capture screen & evaluate dirty rectangles
        bool force_idr = force_next_idr_.exchange(false);
        CapturedFrame frame{};
        if (!capturer_->CaptureNext(frame, force_idr)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            continue;
        }
        MonitorInfo mon = capturer_->CurrentMonitor();
        input_dispatcher_.SetDisplayBounds(
            mon.width > 0 ? mon.width : frame.width,
            mon.height > 0 ? mon.height : frame.height,
            mon.origin_x,
            mon.origin_y);

        NetworkFeedback net_fb{};
        {
            std::lock_guard<std::mutex> lk(state_mu_);
            net_fb.rtt_ms = telemetry_.rtt_ms > 0.0f ? telemetry_.rtt_ms : 5.0f;
            net_fb.loss_pct = std::max(telemetry_.loss_pct, cfg_.simulated_rx_loss_pct);
        }
        auto target = bwe_.Evaluate(net_fb, frame.dirty_ratio);

        // 3. 0-FPS Static Idle optimization: skip video packetization completely when screen is unchanged!
        if (!force_idr && frame.is_static_idle) {
            {
                std::lock_guard<std::mutex> lk(state_mu_);
                ++telemetry_.static_idle_skips;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            continue;
        }

        EncodedAccessUnit au{};
        if (encoder_->Encode(frame, target.target_bitrate_kbps, force_idr, target.prefer_tile_lz4, au) &&
            !au.payload.empty()) {
            // Slice encoded frame into MTU-safe shards
            std::vector<std::vector<uint8_t>> data_shards;
            for (size_t off = 0; off < au.payload.size(); off += kMaxShardData) {
                size_t len = std::min(kMaxShardData, au.payload.size() - off);
                data_shards.emplace_back(au.payload.begin() + off, au.payload.begin() + off + len);
            }

            // Always-on lightweight FEC on keyframes & JPEG frames (0-RTT Wi-Fi loss healing!)
            uint16_t fec_group = target.fec_group_size;
            if (au.is_keyframe || au.codec == VideoCodecId::kJpegFrame) {
                fec_group = (fec_group == 0) ? 4 : std::min<uint16_t>(fec_group, 3);
            }
            auto parities = AdaptiveFecCodec::GenerateParity(data_shards, fec_group);

            uint8_t mon_cnt = capturer_->MonitorCount();
            uint8_t act_mon = static_cast<uint8_t>(mon.index);

            for (uint16_t i = 0; i < data_shards.size(); ++i) {
                PayloadVideoShardHeader vhdr{};
                vhdr.frame_id = au.frame_id;
                vhdr.shard_index = i;
                vhdr.total_data_shards = static_cast<uint16_t>(data_shards.size());
                vhdr.parity_shards = static_cast<uint16_t>(parities.size());
                vhdr.width = au.width;
                vhdr.height = au.height;
                vhdr.codec = au.codec;
                vhdr.is_keyframe = au.is_keyframe ? 1 : 0;
                vhdr.monitor_count = mon_cnt;
                vhdr.active_monitor = act_mon;
                vhdr.dirty_rect_cnt = au.dirty_tile_count;
                vhdr.total_frame_bytes = static_cast<uint32_t>(au.payload.size());
                vhdr.shard_bytes = static_cast<uint16_t>(data_shards[i].size());

                std::vector<uint8_t> pkt(sizeof(vhdr) + data_shards[i].size());
                std::memcpy(pkt.data(), &vhdr, sizeof(vhdr));
                std::memcpy(pkt.data() + sizeof(vhdr), data_shards[i].data(), data_shards[i].size());
                SendEncrypted(QoSLane::kP1_Media, PacketType::kVideoShard, pkt.data(), pkt.size());
            }

            for (const auto& p : parities) {
                PayloadVideoShardHeader phdr{};
                phdr.frame_id = au.frame_id;
                phdr.shard_index = p.group_start_idx;
                phdr.total_data_shards = static_cast<uint16_t>(data_shards.size());
                phdr.parity_shards = static_cast<uint16_t>(parities.size());
                phdr.width = au.width;
                phdr.height = au.height;
                phdr.codec = au.codec;
                phdr.is_keyframe = au.is_keyframe ? 1 : 0;
                phdr.monitor_count = mon_cnt;
                phdr.active_monitor = act_mon;
                phdr.dirty_rect_cnt = p.group_count;
                phdr.total_frame_bytes = static_cast<uint32_t>(au.payload.size());
                phdr.shard_bytes = static_cast<uint16_t>(p.parity_bytes.size());

                std::vector<uint8_t> pkt(sizeof(phdr) + p.parity_bytes.size());
                std::memcpy(pkt.data(), &phdr, sizeof(phdr));
                std::memcpy(pkt.data() + sizeof(phdr), p.parity_bytes.data(), p.parity_bytes.size());
                SendEncrypted(QoSLane::kP1_Media, PacketType::kVideoFecParity, pkt.data(), pkt.size());
            }

            FlushScheduler();
            ++frames_window_;
            {
                std::lock_guard<std::mutex> lk(state_mu_);
                ++telemetry_.frames_sent;
                telemetry_.active_codec = au.codec;
                float enc_ms = static_cast<float>(au.encode_duration_us) / 1000.0f;
                telemetry_.avg_encode_ms = (telemetry_.avg_encode_ms == 0.0f)
                                               ? enc_ms
                                               : (telemetry_.avg_encode_ms * 0.8f + enc_ms * 0.2f);
            }
        }

        uint64_t target_interval_ms = std::max<uint64_t>(7, 1000 / std::max<uint8_t>(15, target.max_fps));
        uint64_t spent_ms = NowMs() - loop_t0;
        if (spent_ms < target_interval_ms) {
            std::this_thread::sleep_for(std::chrono::milliseconds(target_interval_ms - spent_ms));
        }
    }
}

} // namespace uu::core
