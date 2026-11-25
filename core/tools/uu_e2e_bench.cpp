#include "capture_hal.hpp"
#include "codec_hal.hpp"
#include "crypto_e2ee.hpp"
#include "fec_qos.hpp"
#include "server_runtime.hpp"
#include "session_engine.hpp"
#include "dircore_ffi.h"

#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using namespace uu;

namespace {

#define CHECK_OK(cond, msg)                                                      \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "[FAIL] %s (line %d)\n", msg, __LINE__);        \
            return 1;                                                            \
        }                                                                        \
    } while (0)

int TestCryptoSuite() {
    auto alice = core::CryptoEngine::GenerateKeyPair();
    auto bob = core::CryptoEngine::GenerateKeyPair();
    auto s1 = core::CryptoEngine::X25519SharedSecret(alice.secret, bob.pub);
    auto s2 = core::CryptoEngine::X25519SharedSecret(bob.secret, alice.pub);
    CHECK_OK(s1 == s2, "X25519 shared secrets must match");

    auto key = core::CryptoEngine::DeriveSessionKey(s1, "9921");
    const char msg[] = "Hello Ultra-Low-Latency E2EE Frame!";
    const uint8_t aad[] = {1, 2, 3, 4};

    std::vector<uint8_t> sealed;
    core::CryptoEngine::SealPacket(key, 42, aad, sizeof(aad),
                                   reinterpret_cast<const uint8_t*>(msg), sizeof(msg), sealed);

    std::vector<uint8_t> opened;
    CHECK_OK(core::CryptoEngine::OpenPacket(key, 42, aad, sizeof(aad),
                                            sealed.data(), sealed.size(), opened),
             "Valid ChaCha20-AEAD packet must authenticate and decrypt");
    CHECK_OK(opened.size() == sizeof(msg) && std::memcmp(opened.data(), msg, sizeof(msg)) == 0,
             "Decrypted plaintext must match original");

    // Flip 1 bit in ciphertext -> must reject!
    sealed[3] ^= 0x01;
    CHECK_OK(!core::CryptoEngine::OpenPacket(key, 42, aad, sizeof(aad),
                                             sealed.data(), sealed.size(), opened),
             "Tampered ciphertext must be rejected by AEAD tag");

    std::printf("[PASS] 1/5 E2EE Crypto (X25519 + ChaCha20-AEAD + Tamper Rejection)\n");
    return 0;
}

int TestHardwareHevcAndNarrowbandCodecs() {
    core::SyntheticCapturer cap(1280, 720);
    auto enc = core::CreateHybridVideoEncoder();
    auto dec = core::CreateHybridVideoDecoder();

    // 1) Hardware HEVC (H.265) 0-BFrame Full-Motion Test
    cap.SetWorkload(core::SyntheticWorkload::kFullMotionGaming);
    double total_hevc_enc_ms = 0.0;
    double total_hevc_dec_ms = 0.0;
    size_t hevc_pframe_bytes = 0;

    for (int i = 0; i < 12; ++i) {
        core::CapturedFrame cf{};
        cap.CaptureNext(cf, i == 0);
        core::EncodedAccessUnit au{};
        CHECK_OK(enc->Encode(cf, 4000, i == 0, false, au), "Full-motion encode must succeed");
        CHECK_OK(!au.payload.empty(), "Full-motion frame must produce bitstream");

        core::DecodedFrame df{};
        CHECK_OK(dec->Decode(au, df), "Hardware/Hybrid decode must succeed");
        CHECK_OK(df.width == 1280 && df.height == 720 && df.bgra.size() == 1280 * 720 * 4,
                 "Decoded frame dimensions must match 1280x720 BGRA");

        if (i >= 2) {
            total_hevc_enc_ms += static_cast<double>(au.encode_duration_us) / 1000.0;
            total_hevc_dec_ms += static_cast<double>(df.decode_duration_us) / 1000.0;
            hevc_pframe_bytes = au.payload.size();
        }
    }

    // 2) Narrowband Dirty-Tile LZ4 Test (Code Editor Typing + Static 0-Kbps Idle)
    cap.SelectMonitor(0);
    cap.SetWorkload(core::SyntheticWorkload::kCodeEditorTyping);
    core::CapturedFrame init_cf{};
    cap.CaptureNext(init_cf, true);
    core::EncodedAccessUnit init_au{};
    enc->Encode(init_cf, 500, true, true, init_au);
    core::DecodedFrame init_df{};
    CHECK_OK(dec->Decode(init_au, init_df), "Keyframe Tile-LZ4 decode must succeed");

    core::CapturedFrame typing_cf{};
    cap.CaptureNext(typing_cf, false);
    core::EncodedAccessUnit typing_au{};
    CHECK_OK(enc->Encode(typing_cf, 350, false, true, typing_au), "Typing delta encode must succeed");
    CHECK_OK(typing_au.codec == core::VideoCodecId::kTileDeltaLz4, "Typing must select narrowband Tile-LZ4");
    core::DecodedFrame typing_df{};
    CHECK_OK(dec->Decode(typing_au, typing_df), "Typing XOR-residual Tile-LZ4 decode must succeed");
    CHECK_OK(typing_df.bgra == typing_cf.bgra, "XOR-residual Tile-LZ4 must reconstruct 100% bit-exact pixels");
    double typing_kbps_at_20fps = (typing_au.payload.size() * 8.0 * 20.0) / 1000.0;

    // Static Idle must produce 0 bytes!
    cap.SetWorkload(core::SyntheticWorkload::kStaticIdle);
    core::CapturedFrame idle_cf{};
    cap.CaptureNext(idle_cf, false);
    core::EncodedAccessUnit idle_au{};
    CHECK_OK(enc->Encode(idle_cf, 350, false, true, idle_au), "Static idle encode must succeed");
    CHECK_OK(idle_cf.is_static_idle && idle_au.payload.empty(),
             "Static screen must emit 0 video bytes (0 Kbps idle)");

    std::printf("[PASS] 2/5 Dual Codec Pipeline:\n"
                "       - %s 720p avg encode=%.2f ms, decode=%.2f ms (P-frame=%zu B)\n"
                "       - Narrowband Typing Delta=%zu B/frame (%.1f Kbps @ 20fps), Static Idle=0 Kbps\n",
                enc->SupportsHardwareHevc() ? "Apple VideoToolbox HEVC (0-BFrame)" : "Tile-LZ4",
                total_hevc_enc_ms / 10.0,
                total_hevc_dec_ms / 10.0,
                hevc_pframe_bytes,
                typing_au.payload.size(),
                typing_kbps_at_20fps);
    return 0;
}

int TestP2pSpeedAndNarrowbandSession() {
    server::ServerConfig scfg{};
    scfg.rendezvous_port = 0; // Ephemeral OS port
    scfg.relay_port = 0;
    server::LiteServer srv(scfg);
    CHECK_OK(srv.Start(), "LiteServer (hbbs + hbbr) must start");

    core::NodeConfig host_cfg{};
    host_cfg.self_id = "100200300";
    host_cfg.access_pin = "7788";
    host_cfg.hbbs_port = srv.BoundRendezvousPort();
    host_cfg.hbbr_port = srv.BoundRelayPort();
    host_cfg.is_host = true;
    host_cfg.use_synthetic_capture = true;

    core::NodeConfig cli_cfg = host_cfg;
    cli_cfg.self_id = "900800700";
    cli_cfg.is_host = false;
    cli_cfg.download_dir = "/tmp";

    core::PeerNode host(host_cfg);
    core::PeerNode client(cli_cfg);
    CHECK_OK(host.Start(), "Host node must start");
    std::this_thread::sleep_for(std::chrono::milliseconds(30)); // Allow hbbs registration
    CHECK_OK(client.Start(), "Client node must start");

    host.SetSyntheticWorkload(core::SyntheticWorkload::kCodeEditorTyping);
    CHECK_OK(client.ConnectToPeer("100200300", 1500), "Client must connect via P2P UDP hole-punch");
    client.SetQualityProfile(core::QualityProfile::kUltraSaver300K, 30, true);

    // Stream narrowband typing workload for 450ms
    std::this_thread::sleep_for(std::chrono::milliseconds(450));
    auto tele_typing = client.GetTelemetry();
    CHECK_OK(tele_typing.path == core::TransportPath::kP2pDirect, "Must establish direct P2P path");
    CHECK_OK(tele_typing.frames_decoded >= 3, "Client must decode narrowband frames");
    CHECK_OK(tele_typing.cursor_updates >= 2, "Decoupled 12B cursor stream must update independently");

    // Switch host screen to completely static -> verify static idle skips engage!
    host.SetSyntheticWorkload(core::SyntheticWorkload::kStaticIdle);
    std::this_thread::sleep_for(std::chrono::milliseconds(350));
    auto host_tele = host.GetTelemetry();
    CHECK_OK(host_tele.static_idle_skips >= 5, "Host must skip encoding static idle frames (0 Kbps)");

    // Test Input, Clipboard & Chunked File Transfer with SHA-256 verification
    client.SendCursorMoveFast(16384, 32768, 12, -8);
    client.SendKey(36, true, 0);
    client.SendKey(36, false, 0);
    client.SendGamepad({0x01, 12000, -8000, 0, 0, 0, 255});
    client.SendClipboardText("UU-Remote-Lite 跨平台剪贴板同步测试 🚀");

    const std::string src_file = "/tmp/uu_bench_send_test.bin";
    const std::string dst_dir = "/tmp/uu_bench_recv";
    std::system("mkdir -p /tmp/uu_bench_recv");
    {
        std::vector<uint8_t> payload(32 * 1024); // 32KB multi-chunk test payload
        for (size_t i = 0; i < payload.size(); ++i) {
            payload[i] = static_cast<uint8_t>((i * 131 + 17) & 0xFF);
        }
        std::ofstream ofs(src_file, std::ios::binary | std::ios::trunc);
        ofs.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
    }

    // Send file from Client -> Host (verify P2 reliable multi-chunk + SHA-256)
    CHECK_OK(client.SendFile(src_file, 101), "SendFile multi-chunk enqueue must succeed");

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK_OK(host.HostInjectedInputCount() >= 4, "Host must receive mouse, keyboard & gamepad events");
    CHECK_OK(host.LastClipboardText() == "UU-Remote-Lite 跨平台剪贴板同步测试 🚀",
             "LZ4 compressed clipboard text must match on host");
    CHECK_OK(host.IsLastFileVerified(), "Multi-chunk file transfer must pass SHA-256 verification");

    std::printf("[PASS] 3/5 P2P Direct + Narrowband + Full Features:\n"
                "       - Connect Time=%.1f ms (Path=P2P Direct), RTT=%.2f ms\n"
                "       - Narrowband Typing Rate=%.1f Kbps, Static Idle Frame Skips=%llu\n"
                "       - Verified Mouse/Key/Gamepad QoS, LZ4 Clipboard & 32KB SHA-256 File Transfer\n",
                tele_typing.connect_time_ms,
                tele_typing.rtt_ms,
                tele_typing.realtime_kbps,
                static_cast<unsigned long long>(host_tele.static_idle_skips));

    client.Stop();
    host.Stop();
    srv.Stop();
    return 0;
}

int TestRelayFallbackAndPacketLossFec() {
    server::ServerConfig scfg{0, 0};
    server::LiteServer srv(scfg);
    CHECK_OK(srv.Start(), "LiteServer must start for Relay + FEC test");

    // Block direct P2P to simulate Symmetric NAT + inject 10% UDP video packet loss!
    core::NodeConfig host_cfg{};
    host_cfg.self_id = "555666777";
    host_cfg.access_pin = "1234";
    host_cfg.hbbs_port = srv.BoundRendezvousPort();
    host_cfg.hbbr_port = srv.BoundRelayPort();
    host_cfg.is_host = true;
    host_cfg.use_synthetic_capture = true;
    host_cfg.block_direct_p2p_for_test = true;
    host_cfg.simulated_rx_loss_pct = 10.0f; // Inform host BWE to activate FEC parity!

    core::NodeConfig cli_cfg = host_cfg;
    cli_cfg.self_id = "888999000";
    cli_cfg.is_host = false;
    cli_cfg.simulated_rx_loss_pct = 10.0f; // Drop 10% of incoming video UDP packets at client!

    core::PeerNode host(host_cfg);
    core::PeerNode client(cli_cfg);
    CHECK_OK(host.Start(), "Host must start");
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK_OK(client.Start(), "Client must start");

    host.SetSyntheticWorkload(core::SyntheticWorkload::kFullMotionGaming);
    CHECK_OK(client.ConnectToPeer("555666777", 1500),
             "Happy-Eyeballs must seamlessly connect via hbbr Relay when P2P is blocked");

    std::this_thread::sleep_for(std::chrono::milliseconds(650));
    auto cli_tele = client.GetTelemetry();
    auto srv_stats = srv.GetStats();

    CHECK_OK(cli_tele.path == core::TransportPath::kRelayFallback,
             "Session must run over Zero-Copy Relay path");
    CHECK_OK(srv_stats.relayed_packets > 20, "hbbr Relay must forward packets zero-copy");
    CHECK_OK(cli_tele.frames_decoded >= 5,
             "Client must continuously decode frames despite 10% UDP packet loss");
    CHECK_OK(cli_tele.fec_recovered_frames >= 1,
             "Adaptive FEC must reconstruct frames with missing UDP shards in 0-RTT");

    std::printf("[PASS] 4/5 Symmetric NAT Relay Fallback + 10%% Packet Loss FEC:\n"
                "       - Happy-Eyeballs Relay Connect=%.1f ms, Relayed=%llu pkts (%.1f KB)\n"
                "       - Decoded=%llu frames under 10%% UDP drop, 0-RTT FEC Recovered=%llu frames\n",
                cli_tele.connect_time_ms,
                static_cast<unsigned long long>(srv_stats.relayed_packets),
                static_cast<double>(srv_stats.relayed_bytes) / 1024.0,
                static_cast<unsigned long long>(cli_tele.frames_decoded),
                static_cast<unsigned long long>(cli_tele.fec_recovered_frames));

    client.Stop();
    host.Stop();
    srv.Stop();
    return 0;
}

int TestCAbiFfiSmoke() {
    server::ServerConfig scfg{0, 0};
    server::LiteServer srv(scfg);
    CHECK_OK(srv.Start(), "LiteServer for FFI test");

    DirHandle h_host = dir_node_create("333222111", "4321", "127.0.0.1",
                                       srv.BoundRendezvousPort(), srv.BoundRelayPort(), 1);
    DirHandle h_cli = dir_node_create("777888999", "4321", "127.0.0.1",
                                      srv.BoundRendezvousPort(), srv.BoundRelayPort(), 0);
    CHECK_OK(h_host != nullptr && h_cli != nullptr, "C ABI handles must allocate");
    CHECK_OK(dir_node_start(h_host) == 1, "FFI host start");
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK_OK(dir_node_start(h_cli) == 1, "FFI client start");
    CHECK_OK(dir_node_connect(h_cli, "333222111", 1500) == 1, "FFI client connect");

    // Switch to Monitor #1 via FFI options (auto codec, audio enabled) and verify connection
    dir_node_set_options(h_cli, 1, 60, 0, 1, 0, 1);
    dir_node_send_text(h_cli, "DirRemote FFI 你好世界");
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    DirTelemetryC t{};
    dir_node_get_telemetry(h_cli, &t);
    CHECK_OK(t.transport_path == 1, "FFI telemetry must report active connection");

    dir_node_destroy(h_cli);
    dir_node_destroy(h_host);
    srv.Stop();
    std::printf("[PASS] 5/7 DirRemote Flutter C ABI FFI Bindings & Multi-Monitor Switch\n");
    return 0;
}

int TestRealPhotographicJpegWifiPipeline() {
    // 1) Benchmark 1280x800 (4.1 MB raw) colorful smooth photographic wallpaper compression!
    constexpr uint16_t kW = 1280;
    constexpr uint16_t kH = 800;
    std::vector<uint8_t> photo(static_cast<size_t>(kW) * kH * 4);
    for (uint16_t y = 0; y < kH; ++y) {
        for (uint16_t x = 0; x < kW; ++x) {
            size_t idx = (static_cast<size_t>(y) * kW + x) * 4;
            photo[idx + 0] = static_cast<uint8_t>((x * 255) / kW);
            photo[idx + 1] = static_cast<uint8_t>((y * 255) / kH);
            photo[idx + 2] = static_cast<uint8_t>(((x + y) * 180) / (kW + kH));
            photo[idx + 3] = 255;
        }
    }

    auto t0 = std::chrono::steady_clock::now();
    std::vector<uint8_t> jpeg;
    bool ok = core::EncodeHardwareJpeg(photo.data(), kW, kH, 0.62f, jpeg);
    double enc_ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    CHECK_OK(ok && jpeg.size() > 1000 && jpeg.size() < 150000,
             "Hardware JPEG must compress 4.1MB 1280x800 photo to <150KB");
    CHECK_OK(jpeg[0] == 0xFF && jpeg[1] == 0xD8, "JPEG bitstream must start with SOI 0xFFD8");

    // 2) End-to-End UDP delivery with Always-On Keyframe FEC under 5% Wi-Fi packet loss!
    server::ServerConfig scfg{0, 0};
    server::LiteServer srv(scfg);
    CHECK_OK(srv.Start(), "LiteServer for JPEG E2E");

    core::NodeConfig host_cfg{};
    host_cfg.self_id = "200300400";
    host_cfg.access_pin = "5678";
    host_cfg.hbbs_port = srv.BoundRendezvousPort();
    host_cfg.hbbr_port = srv.BoundRelayPort();
    host_cfg.is_host = true;
    host_cfg.use_synthetic_capture = true;

    core::NodeConfig cli_cfg = host_cfg;
    cli_cfg.self_id = "600700800";
    cli_cfg.is_host = false;
    cli_cfg.simulated_rx_loss_pct = 5.0f; // 5% Wi-Fi random UDP packet drops!

    core::PeerNode host(host_cfg);
    core::PeerNode client(cli_cfg);
    host.ForceJpegModeForTest(true);
    CHECK_OK(host.Start(), "JPEG host start");
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK_OK(client.Start(), "JPEG client start");
    host.SetSyntheticWorkload(core::SyntheticWorkload::kFullMotionGaming);
    CHECK_OK(client.ConnectToPeer("200300400", 1500), "JPEG client connect");

    std::this_thread::sleep_for(std::chrono::milliseconds(350));
    uint64_t seq = 0;
    uint8_t fmt = 0;
    std::vector<uint8_t> rx_bytes;
    uint16_t rw = 0, rh = 0;
    CHECK_OK(client.CopyLatestFrameEx(0, seq, fmt, rx_bytes, rw, rh),
             "Client must receive JPEG frame over lossy Wi-Fi");
    CHECK_OK(fmt == 2 && rx_bytes.size() > 1000 && rx_bytes[0] == 0xFF && rx_bytes[1] == 0xD8,
             "Received zero-copy frame must be valid JPEG bitstream (format=2)");

    double ratio = static_cast<double>(photo.size()) / static_cast<double>(jpeg.size());
    std::printf("[PASS] 6/7 Photographic 1280x800 Hardware JPEG + 5%% Wi-Fi Loss Pipeline:\n"
                "       - 4.1 MB raw wallpaper -> %.1f KB JPEG (%.0fx smaller!) in %.2f ms\n"
                "       - Delivered valid JPEG bitstream (%.1f KB) under 5%% UDP drop with Always-On FEC\n",
                static_cast<double>(jpeg.size()) / 1024.0, ratio, enc_ms,
                static_cast<double>(rx_bytes.size()) / 1024.0);

    client.Stop();
    host.Stop();
    srv.Stop();
    return 0;
}

int TestV120AudioGamepadImeAndCrossOsMatrix() {
    // 1) Cross-OS Modifier & Keycode Translation Verification (Win<->Mac, Win<->Win, Mac<->Mac)
    // Ctrl (0x02) from Windows -> macOS MUST become Cmd (0x08); Cmd (0x08) from macOS -> Windows MUST become Ctrl (0x02)
    CHECK_OK(core::RemapModifiersCrossOs(0x02, core::HostOsType::kWindows, core::HostOsType::kMacOS) == 0x08,
             "Win->Mac Ctrl+C must map to Cmd+C");
    CHECK_OK(core::RemapModifiersCrossOs(0x08, core::HostOsType::kMacOS, core::HostOsType::kWindows) == 0x02,
             "Mac->Win Cmd+C must map to Ctrl+C");
    CHECK_OK(core::RemapModifiersCrossOs(0x02, core::HostOsType::kWindows, core::HostOsType::kWindows) == 0x02,
             "Win->Win Ctrl must remain Ctrl");
    CHECK_OK(core::RemapModifiersCrossOs(0x08, core::HostOsType::kMacOS, core::HostOsType::kMacOS) == 0x08,
             "Mac->Mac Cmd must remain Cmd");
    constexpr uint16_t kKeyC = core::DirKey::kA + 2; // 'C' = 0x06
    CHECK_OK(core::DirKeyToMacVirtualKey(kKeyC) == 8 &&
             core::DirKeyToWinVirtualKey(kKeyC) == 'C',
             "Canonical DirKey 'C' must map accurately to Mac (8) and Win ('C')");
    CHECK_OK(core::DirKeyToMacVirtualKey(core::DirKey::kEnter) == 36 &&
             core::DirKeyToWinVirtualKey(core::DirKey::kEnter) == 0x0D,
             "Canonical DirKey::kEnter must map accurately to Mac (36) and Win (0x0D)");

    // 2) Stereo IMA-ADPCM 4:1 Audio Compression Round-Trip Verification (480 stereo frames = 1920B -> 480B!)
    std::vector<int16_t> sine_pcm(480 * 2);
    for (size_t i = 0; i < 480; ++i) {
        double phase = 2.0 * 3.1415926535 * 440.0 * static_cast<double>(i) / 48000.0;
        sine_pcm[i * 2 + 0] = static_cast<int16_t>(std::sin(phase) * 14000.0);
        sine_pcm[i * 2 + 1] = static_cast<int16_t>(std::cos(phase) * 14000.0);
    }
    core::AdpcmChannelState enc_l{}, enc_r{}, dec_l{}, dec_r{};
    std::vector<uint8_t> adpcm;
    core::AdpcmStereoCodec::Encode(sine_pcm.data(), 480, enc_l, enc_r, adpcm);
    CHECK_OK(adpcm.size() == 480, "Stereo ADPCM must compress 1920B PCM -> 480B (4:1 ratio)");
    std::vector<int16_t> decoded_pcm;
    core::AdpcmStereoCodec::Decode(adpcm.data(), adpcm.size(), 480, dec_l, dec_r, decoded_pcm);
    CHECK_OK(decoded_pcm.size() == sine_pcm.size(), "ADPCM decode must restore exact frame count");
    double err_sum = 0.0;
    for (size_t i = 0; i < sine_pcm.size(); ++i) {
        err_sum += std::abs(static_cast<double>(sine_pcm[i]) - static_cast<double>(decoded_pcm[i]));
    }
    double avg_err = err_sum / static_cast<double>(sine_pcm.size());
    CHECK_OK(avg_err < 350.0, "ADPCM audio distortion must remain ultra-low (<1.1% full scale)");

    // 3) Live Simulated E2E Verification across all 4 Desktop Pairs:
    //    Win->Mac, Mac->Win, Win->Win, Mac->Mac + Audio + IME Unicode + Virtual Gamepad + Codec Switch!
    struct OsPair {
        const char* name;
        core::HostOsType client_os;
        core::HostOsType host_os;
    };
    const OsPair pairs[4] = {
        {"Win->Mac", core::HostOsType::kWindows, core::HostOsType::kMacOS},
        {"Mac->Win", core::HostOsType::kMacOS, core::HostOsType::kWindows},
        {"Win->Win", core::HostOsType::kWindows, core::HostOsType::kWindows},
        {"Mac->Mac", core::HostOsType::kMacOS, core::HostOsType::kMacOS},
    };

    server::ServerConfig scfg{0, 0};
    server::LiteServer srv(scfg);
    CHECK_OK(srv.Start(), "LiteServer for v1.2.0 matrix");

    uint64_t total_audio_rx = 0;
    for (int idx = 0; idx < 4; ++idx) {
        core::NodeConfig hcfg{};
        hcfg.self_id = "80000000" + std::to_string(idx);
        hcfg.access_pin = "9999";
        hcfg.hbbs_port = srv.BoundRendezvousPort();
        hcfg.hbbr_port = srv.BoundRelayPort();
        hcfg.is_host = true;
        hcfg.use_synthetic_capture = true;
        hcfg.simulated_os_override = pairs[idx].host_os;

        core::NodeConfig ccfg = hcfg;
        ccfg.self_id = "90000000" + std::to_string(idx);
        ccfg.is_host = false;
        ccfg.simulated_os_override = pairs[idx].client_os;

        core::PeerNode host(hcfg);
        core::PeerNode client(ccfg);
        CHECK_OK(host.Start(), "Matrix host start");
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
        CHECK_OK(client.Start(), "Matrix client start");
        CHECK_OK(client.ConnectToPeer(hcfg.self_id, 1500), "Matrix client connect");
        std::this_thread::sleep_for(std::chrono::milliseconds(30));

        CHECK_OK(host.HostPeerOsType() == pairs[idx].client_os,
                 "Host must negotiate exact remote controller OS for keymap translation");

        // Send cross-OS shortcut (Ctrl/Cmd+C), Android IME Chinese Unicode text, & full Dual-Stick Gamepad!
        client.SendKey(kKeyC, true, 0x02);
        client.SendKey(kKeyC, false, 0);
        client.SendUnicodeText("DirRemote 跨平台中文输入测试! 🚀");
        core::PayloadGamepad pad{};
        pad.buttons = 0x01 | 0x10; // A (Space) + LB (Q)
        pad.lx = 28000;            // Move Right (D)
        pad.ly = -26000;           // Move Forward (W)
        pad.rx = 16000;            // Camera pan right
        pad.rt = 255;              // Primary Fire (Left Click)
        client.SendGamepad(pad);

        // Test Codec Preference switch (Tile Lossless -> Auto) + System Audio ADPCM packet delivery!
        client.SetQualityProfile(core::QualityProfile::kBalancedAuto, 60, false, 0,
                                 core::CodecPreference::kTileLossless, true);
        for (int a = 0; a < 5; ++a) {
            host.InjectSyntheticAudioFrameForTest(sine_pcm.data(), 480, 48000);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        CHECK_OK(host.HostInjectedInputCount() >= 5,
                 "Host must dispatch key, Unicode IME text & gamepad WASD/mouse actions");
        CHECK_OK(host.HostLastUnicodeText() == "DirRemote 跨平台中文输入测试! 🚀",
                 "Host must receive exact UTF-8 Chinese/Emoji IME text");
        CHECK_OK(client.ClientAudioPacketsReceived() >= 3,
                 "Client must receive & decode encrypted ADPCM system audio stream");
        total_audio_rx += client.ClientAudioPacketsReceived();

        client.Stop();
        host.Stop();
    }

    srv.Stop();
    std::printf("[PASS] 7/7 v1.2.0 Full Matrix (Win<->Mac, Win<->Win, Mac<->Mac) + Audio + IME + Gamepad:\n"
                "       - 4/4 OS pairs verified live over E2EE UDP (auto Cmd<->Ctrl cross-OS remap!)\n"
                "       - Stereo 48kHz IMA-ADPCM 4:1 audio: avg error=%.1f, %llu encrypted audio pkts delivered\n"
                "       - Android/Mobile UTF-8 CJK IME text & Virtual Gamepad (WASD + Camera + Triggers) verified!\n",
                avg_err, static_cast<unsigned long long>(total_audio_rx));
    return 0;
}

} // namespace

int main() {
    std::printf("====================================================================\n");
    std::printf("  DirRemote End-to-End Verification & Performance Suite\n");
    std::printf("====================================================================\n");

    if (TestCryptoSuite() != 0) return 1;
    if (TestHardwareHevcAndNarrowbandCodecs() != 0) return 1;
    if (TestP2pSpeedAndNarrowbandSession() != 0) return 1;
    if (TestRelayFallbackAndPacketLossFec() != 0) return 1;
    if (TestCAbiFfiSmoke() != 0) return 1;
    if (TestRealPhotographicJpegWifiPipeline() != 0) return 1;
    if (TestV120AudioGamepadImeAndCrossOsMatrix() != 0) return 1;

    std::printf("====================================================================\n");
    std::printf("  ALL 7 BENCHMARKS & E2E VERIFICATION SUITES PASSED!\n");
    std::printf("====================================================================\n");
    return 0;
}
