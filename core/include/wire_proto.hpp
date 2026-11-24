#pragma once

#include <cstddef>
#include <cstdint>

namespace uu::core {

// Wire Magic "UUP2" (0x55555032)
constexpr uint32_t kWireMagic = 0x55555032;
constexpr size_t kMaxUdpMtuPayload = 1180; // Safe across PPPoE, VPN, 4G/5G cellular & TURN/Relay

// Strict 3-Tier QoS Priority Lanes
enum class QoSLane : uint8_t {
    kP0_FastInput = 0, // Highest priority: Cursor XY (12B), mouse delta, gamepad axes (unreliable, 0 head-of-line blocking)
    kP1_Media     = 1, // Real-time Video (HEVC / Dirty-Tile LZ4), Audio, Cursor shape bitmap (FEC protected)
    kP2_Reliable  = 2, // Keyboard state, Clipboard sync, Chunked File Transfer, Session/Display control (ACK + Retransmit)
};

enum class PacketType : uint8_t {
    // Handshake & Keepalive
    kHelloInit       = 0x01, // Initiator X25519 ephemeral pubkey + auth proof
    kHelloAccept     = 0x02, // Responder X25519 ephemeral pubkey + display/codec capabilities
    kPing            = 0x03, // RTT & BWE probe
    kPong            = 0x04, // Echo timestamp + receiver loss/jitter report

    // P0: Ultra-low-latency decoupled cursor & analog input
    kCursorPos       = 0x10, // 12-byte payload: normalized/pixel X, Y, buttons, cursor_shape_id
    kMouseFast       = 0x11, // Relative/Absolute mouse move + wheel
    kGamepadState    = 0x12, // Xbox/Standard virtual controller state (buttons + 6 axes)
    kRequestIdr      = 0x13, // Instant PLI (Picture Loss Indication) keyframe recovery request

    // P1: Adaptive Media & Cursor Shape Cache
    kVideoShard      = 0x20, // Video data shard (HEVC or Tile-LZ4)
    kVideoFecParity  = 0x21, // Adaptive FEC parity shard for 0-RTT loss recovery
    kCursorShape     = 0x22, // Sent ONLY when cursor shape hash changes (LZ4 compressed RGBA)
    kAudioFrame      = 0x23, // Opus/PCM low-latency audio frame

    // P2: Ordered Reliable Control, Clipboard & File Transfer
    kAck             = 0x30, // Selective ACK for P2 reliable sequence numbers
    kKeyInput        = 0x31, // Scancode press/release + modifier bitmask (anti-stuck-key)
    kMouseClick      = 0x32, // Reliable button press/release
    kQualityControl  = 0x33, // Switch QualityMode, FPS cap, Privacy Screen, Monitor index
    kClipboardSync   = 0x34, // Compressed text/image clipboard payload
    kFileOffer       = 0x35, // File transfer metadata (file_id, total_size, sha256, resume_offset)
    kFileChunk       = 0x36, // Resumable chunk payload
    kFileComplete    = 0x37, // Checksum verification result
};

enum class QualityProfile : uint8_t {
    kUltraSaver300K  = 0, // Low-bandwidth mode: 250Kbps~1Mbps, 720p/900p scale, VFR 0~30fps, Tile-LZ4 / HEVC high QP
    kBalancedAuto    = 1, // Smart hybrid: auto-scales 500Kbps~15Mbps, VFR 0~60fps, seamless Tile-LZ4 <-> HEVC switch
    kUltraGaming144  = 2, // High-speed mode: up to 100Mbps, native resolution, 120/144fps, HEVC 0-BFrame low-latency
};

enum class VideoCodecId : uint8_t {
    kTileDeltaLz4 = 1, // Zero-dependency dirty-rect lossless/near-lossless narrowband desktop codec (0 Kbps when idle)
    kHevcHardware = 2, // Hardware H.265 / HEVC (Apple VideoToolbox / Win MediaFoundation-NVENC / Android MediaCodec)
    kAv1Hardware  = 3, // Hardware AV1 when mutually supported
};

#pragma pack(push, 1)

// Compact 16-byte encrypted wire envelope header (authenticated as AEAD AAD)
struct WireHeader {
    uint32_t magic;        // kWireMagic
    PacketType type;       // PacketType
    QoSLane lane;          // QoSLane (0, 1, 2)
    uint16_t payload_len;  // Length of encrypted payload (excluding 16B Poly1305 tag when E2EE active)
    uint32_t seq;          // Monotonic packet sequence / nonce counter
    uint32_t ack_or_group; // For P2: reliable seq/ack; For P1 Video: FEC group_id (upper 24b) + shard_idx (lower 8b)
};

// 12-byte decoupled cursor packet -> sub-millisecond local pointer rendering even at 200Kbps!
struct PayloadCursorPos {
    uint16_t x;            // Normalized 0..65535 across active display width
    uint16_t y;            // Normalized 0..65535 across active display height
    uint64_t shape_hash;   // 64-bit FNV-1a hash of cursor RGBA bitmap (client looks up local cache)
};

struct PayloadCursorShapeHeader {
    uint64_t shape_hash;
    uint16_t width;
    uint16_t height;
    uint16_t hotspot_x;
    uint16_t hotspot_y;
    uint16_t compressed_len; // Followed by LZ4-compressed BGRA pixels
};

struct PayloadVideoShardHeader {
    uint32_t frame_id;
    uint16_t shard_index;
    uint16_t total_data_shards;
    uint16_t parity_shards;
    uint16_t width;
    uint16_t height;
    VideoCodecId codec;
    uint8_t is_keyframe;     // 1 = full frame / IDR, 0 = delta / P-frame
    uint16_t dirty_rect_cnt; // Number of changed 64x64 tiles
    uint32_t total_frame_bytes;
    uint16_t shard_bytes;
};

struct PayloadDirtyTileHeader {
    uint16_t tile_x; // In pixels
    uint16_t tile_y; // In pixels
    uint16_t tile_w;
    uint16_t tile_h;
    uint16_t raw_or_lz4_flag; // 0 = raw, 1 = LZ4 compressed
    uint16_t data_len;
};

struct PayloadPingPong {
    uint64_t send_ts_us;
    uint32_t rx_packets_window;
    uint32_t lost_packets_window;
    uint32_t jitter_us;
};

struct PayloadKeyInput {
    uint16_t keycode;
    uint8_t is_down;
    uint8_t modifiers; // Bit 0: Shift, 1: Ctrl, 2: Alt/Opt, 3: Meta/Cmd/Win
};

struct PayloadMouseEvent {
    uint16_t x_norm;
    uint16_t y_norm;
    int16_t dx;
    int16_t dy;
    int16_t wheel_y;
    uint8_t button_mask; // Bit 0: Left, 1: Right, 2: Middle
    uint8_t is_relative; // 1 = FPS/Trackpad relative delta, 0 = Absolute touch/pointer
};

struct PayloadGamepad {
    uint16_t buttons;  // Xbox layout bitmask (A,B,X,Y,LB,RB,Back,Start,LS,RS,DPad)
    int16_t lx, ly;    // -32768..32767
    int16_t rx, ry;
    uint8_t lt, rt;    // 0..255 triggers
};

struct PayloadQualityControl {
    QualityProfile profile;
    uint8_t target_fps;       // e.g., 30, 60, 120, 144
    uint8_t monitor_index;    // Multi-monitor switch
    uint8_t privacy_curtain;  // 1 = black out host physical display
    uint32_t max_bitrate_kbps;
};

struct PayloadFileOffer {
    uint64_t file_id;
    uint64_t total_bytes;
    uint64_t resume_offset;
    uint8_t sha256[32];
    char filename[128];
};

struct PayloadFileChunkHeader {
    uint64_t file_id;
    uint64_t offset;
    uint16_t chunk_len;
};

#pragma pack(pop)

} // namespace uu::core
