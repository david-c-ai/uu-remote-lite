#pragma once

#include "keymap_hal.hpp"
#include "wire_proto.hpp"
#include <array>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace uu::core {

class InputDispatcher {
public:
    explicit InputDispatcher(bool enable_os_injection = false);

    void SetOsInjectionEnabled(bool enabled) { enable_os_injection_ = enabled; }
    void SetPeerOsType(HostOsType peer_os) { peer_os_ = peer_os; }
    HostOsType PeerOsType() const { return peer_os_; }
    void SetDisplayBounds(uint16_t width, uint16_t height, int32_t origin_x = 0, int32_t origin_y = 0);
    void ApplyMouse(const PayloadMouseEvent& ev);
    void ApplyKey(const PayloadKeyInput& ev);
    void ApplyUnicodeText(const std::string& utf8_text);
    void ApplyGamepad(const PayloadGamepad& pad);

    // Releases any depressed keys on session teardown so host keyboard never gets stuck!
    void ReleaseAllHeldKeys();

    uint64_t InjectedEventCount() const { return injected_count_; }
    uint64_t InjectedTextCount() const { return injected_text_count_; }
    const std::string& LastUnicodeText() const { return last_unicode_text_; }
    PayloadGamepad LastGamepadState() const { return last_pad_; }

private:
    void SetGamepadKey(uint16_t dir_key, bool want_down);

    bool enable_os_injection_ = false;
    HostOsType peer_os_ = HostOsType::kUnknown;
    int32_t origin_x_ = 0;
    int32_t origin_y_ = 0;
    uint16_t display_w_ = 1920;
    uint16_t display_h_ = 1080;
    uint8_t prev_buttons_ = 0;
    uint64_t injected_count_ = 0;
    uint64_t injected_text_count_ = 0;
    std::string last_unicode_text_;
    PayloadGamepad last_pad_{};
    std::unordered_set<uint16_t> held_keys_;
    std::unordered_set<uint16_t> pad_held_keys_;
};

class ResumableFileEngine {
public:
    static constexpr size_t kDefaultChunkBytes = 960; // Fits inside single UDP P2 MTU packet!

    // Sender side: prepare file offer from memory/file path
    bool PrepareSendFromFile(const std::string& path, uint64_t file_id, PayloadFileOffer& out_offer);

    // Sender side: slice chunks starting at receiver's resume_offset
    bool ReadSendChunk(uint64_t file_id, uint64_t offset, std::vector<uint8_t>& out_chunk_packet);

    // Receiver side: register incoming file offer in destination directory, returns resume_offset
    uint64_t AcceptIncomingOffer(const PayloadFileOffer& offer, const std::string& dest_dir);

    // Receiver side: write incoming chunk; sets out_completed = true when total_bytes reached & SHA-256 verified!
    bool OnIncomingChunk(const PayloadFileChunkHeader& hdr,
                         const uint8_t* data,
                         bool& out_completed,
                         bool& out_sha256_ok);

    // Clipboard LZ4 pack / unpack helpers
    static std::vector<uint8_t> PackClipboardText(const std::string& utf8_text);
    static std::string UnpackClipboardText(const uint8_t* data, size_t len);

private:
    struct OutboundFile {
        std::string path;
        std::vector<uint8_t> bytes;
        std::array<uint8_t, 32> sha256{};
    };

    struct InboundFile {
        std::string final_path;
        uint64_t total_bytes = 0;
        uint64_t received_bytes = 0;
        std::array<uint8_t, 32> expected_sha256{};
        std::vector<uint8_t> buffer;
    };

    std::mutex mu_;
    std::unordered_map<uint64_t, OutboundFile> outbound_;
    std::unordered_map<uint64_t, InboundFile> inbound_;
};

} // namespace uu::core
