#include "hid_clipboard_file.hpp"

#include "codec_hal.hpp"
#include "crypto_e2ee.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_OSX
#include <ApplicationServices/ApplicationServices.h>
#endif
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace uu::core {

InputDispatcher::InputDispatcher(bool enable_os_injection)
    : enable_os_injection_(enable_os_injection) {}

void InputDispatcher::SetDisplayBounds(uint16_t width, uint16_t height, int32_t origin_x, int32_t origin_y) {
    origin_x_ = origin_x;
    origin_y_ = origin_y;
    display_w_ = std::max<uint16_t>(1, width);
    display_h_ = std::max<uint16_t>(1, height);
}

void InputDispatcher::ApplyMouse(const PayloadMouseEvent& ev) {
    ++injected_count_;
    uint8_t changed = prev_buttons_ ^ ev.button_mask;
    prev_buttons_ = ev.button_mask;
    if (!enable_os_injection_) return;

#if defined(__APPLE__) && TARGET_OS_OSX
    double px = static_cast<double>(origin_x_) + (static_cast<double>(ev.x_norm) / 65535.0) * display_w_;
    double py = static_cast<double>(origin_y_) + (static_cast<double>(ev.y_norm) / 65535.0) * display_h_;
    CGPoint pt = CGPointMake(px, py);

    auto post_btn = [&](CGEventType type, CGMouseButton btn) {
        CGEventRef ce = CGEventCreateMouseEvent(nullptr, type, pt, btn);
        if (ce) {
            CGEventPost(kCGHIDEventTap, ce);
            CFRelease(ce);
        }
    };

    if (changed & 0x01) {
        post_btn((ev.button_mask & 0x01) ? kCGEventLeftMouseDown : kCGEventLeftMouseUp, kCGMouseButtonLeft);
    } else if (changed & 0x02) {
        post_btn((ev.button_mask & 0x02) ? kCGEventRightMouseDown : kCGEventRightMouseUp, kCGMouseButtonRight);
    } else {
        CGEventType mtype = kCGEventMouseMoved;
        CGMouseButton btn = kCGMouseButtonLeft;
        if (ev.button_mask & 0x01) {
            mtype = kCGEventLeftMouseDragged;
        } else if (ev.button_mask & 0x02) {
            mtype = kCGEventRightMouseDragged;
            btn = kCGMouseButtonRight;
        }
        post_btn(mtype, btn);
    }

    if (ev.wheel_y != 0) {
        CGEventRef wheel = CGEventCreateScrollWheelEvent(
            nullptr, kCGScrollEventUnitPixel, 1, static_cast<int32_t>(ev.wheel_y));
        if (wheel) {
            CGEventPost(kCGHIDEventTap, wheel);
            CFRelease(wheel);
        }
    }
#elif defined(_WIN32)
    INPUT inp{};
    inp.type = INPUT_MOUSE;
    if (ev.is_relative && changed == 0) {
        inp.mi.dx = ev.dx;
        inp.mi.dy = ev.dy;
        inp.mi.dwFlags = MOUSEEVENTF_MOVE;
    } else {
        int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
        int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
        int vw = std::max(1, GetSystemMetrics(SM_CXVIRTUALSCREEN));
        int vh = std::max(1, GetSystemMetrics(SM_CYVIRTUALSCREEN));
        double global_x = origin_x_ + (static_cast<double>(ev.x_norm) / 65535.0) * display_w_;
        double global_y = origin_y_ + (static_cast<double>(ev.y_norm) / 65535.0) * display_h_;
        inp.mi.dx = static_cast<LONG>(((global_x - vx) * 65535.0) / vw);
        inp.mi.dy = static_cast<LONG>(((global_y - vy) * 65535.0) / vh);
        inp.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    }
    if (changed & 0x01) {
        inp.mi.dwFlags |= (ev.button_mask & 0x01) ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    }
    if (changed & 0x02) {
        inp.mi.dwFlags |= (ev.button_mask & 0x02) ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
    }
    if (ev.wheel_y != 0) {
        inp.mi.dwFlags |= MOUSEEVENTF_WHEEL;
        inp.mi.mouseData = static_cast<DWORD>(ev.wheel_y);
    }
    SendInput(1, &inp, sizeof(INPUT));
#endif
}

void InputDispatcher::ApplyKey(const PayloadKeyInput& ev) {
    ++injected_count_;
    if (ev.is_down) {
        held_keys_.insert(ev.keycode);
    } else {
        held_keys_.erase(ev.keycode);
    }
    if (!enable_os_injection_) return;

#if defined(__APPLE__) && TARGET_OS_OSX
    CGEventRef kev = CGEventCreateKeyboardEvent(
        nullptr, static_cast<CGKeyCode>(ev.keycode), ev.is_down != 0);
    if (kev) {
        CGEventPost(kCGHIDEventTap, kev);
        CFRelease(kev);
    }
#elif defined(_WIN32)
    INPUT inp{};
    inp.type = INPUT_KEYBOARD;
    inp.ki.wVk = ev.keycode;
    inp.ki.dwFlags = ev.is_down ? 0 : KEYEVENTF_KEYUP;
    SendInput(1, &inp, sizeof(INPUT));
#endif
}

void InputDispatcher::ApplyGamepad(const PayloadGamepad& pad) {
    ++injected_count_;
    last_pad_ = pad;
}

void InputDispatcher::ReleaseAllHeldKeys() {
    std::vector<uint16_t> keys(held_keys_.begin(), held_keys_.end());
    for (uint16_t code : keys) {
        PayloadKeyInput rel{code, 0, 0};
        ApplyKey(rel);
    }
    held_keys_.clear();
}

// --- Resumable Chunked File Transfer & Compressed Clipboard ---

bool ResumableFileEngine::PrepareSendFromFile(const std::string& path, uint64_t file_id,
                                              PayloadFileOffer& out_offer) {
    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs) return false;
    auto sz = ifs.tellg();
    if (sz < 0) return false;
    ifs.seekg(0, std::ios::beg);

    OutboundFile entry;
    entry.path = path;
    entry.bytes.resize(static_cast<size_t>(sz));
    if (sz > 0) {
        ifs.read(reinterpret_cast<char*>(entry.bytes.data()), sz);
    }
    entry.sha256 = CryptoEngine::Sha256(entry.bytes.data(), entry.bytes.size());

    std::memset(&out_offer, 0, sizeof(out_offer));
    out_offer.file_id = file_id;
    out_offer.total_bytes = entry.bytes.size();
    out_offer.resume_offset = 0;
    std::memcpy(out_offer.sha256, entry.sha256.data(), 32);

    size_t slash = path.find_last_of("/\\");
    std::string fname = (slash == std::string::npos) ? path : path.substr(slash + 1);
    std::strncpy(out_offer.filename, fname.c_str(), sizeof(out_offer.filename) - 1);

    std::lock_guard<std::mutex> lk(mu_);
    outbound_[file_id] = std::move(entry);
    return true;
}

bool ResumableFileEngine::ReadSendChunk(uint64_t file_id, uint64_t offset,
                                        std::vector<uint8_t>& out_chunk_packet) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = outbound_.find(file_id);
    if (it == outbound_.end()) return false;
    const auto& data = it->second.bytes;
    if (offset >= data.size()) return false;

    size_t len = std::min<size_t>(kDefaultChunkBytes, data.size() - static_cast<size_t>(offset));
    PayloadFileChunkHeader hdr{};
    hdr.file_id = file_id;
    hdr.offset = offset;
    hdr.chunk_len = static_cast<uint16_t>(len);

    out_chunk_packet.resize(sizeof(hdr) + len);
    std::memcpy(out_chunk_packet.data(), &hdr, sizeof(hdr));
    std::memcpy(out_chunk_packet.data() + sizeof(hdr), data.data() + offset, len);
    return true;
}

uint64_t ResumableFileEngine::AcceptIncomingOffer(const PayloadFileOffer& offer,
                                                  const std::string& dest_dir) {
    std::lock_guard<std::mutex> lk(mu_);
    std::string fname(offer.filename, strnlen(offer.filename, sizeof(offer.filename)));
    // Sanitize filename against path traversal
    size_t slash = fname.find_last_of("/\\");
    if (slash != std::string::npos) fname = fname.substr(slash + 1);
    if (fname.empty()) fname = "received_file.bin";

    std::string full_path = dest_dir + "/" + fname;
    std::string part_path = full_path + ".uupart";

    InboundFile inb;
    inb.final_path = full_path;
    inb.total_bytes = offer.total_bytes;
    std::memcpy(inb.expected_sha256.data(), offer.sha256, 32);
    inb.buffer.assign(static_cast<size_t>(offer.total_bytes), 0);

    // Check if a partial .uupart file exists for instant breakpoint resume!
    std::ifstream part_in(part_path, std::ios::binary | std::ios::ate);
    if (part_in) {
        auto existing = part_in.tellg();
        if (existing > 0 && static_cast<uint64_t>(existing) < offer.total_bytes) {
            part_in.seekg(0, std::ios::beg);
            part_in.read(reinterpret_cast<char*>(inb.buffer.data()), existing);
            inb.received_bytes = static_cast<uint64_t>(existing);
        }
    }

    uint64_t resume_pos = inb.received_bytes;
    inbound_[offer.file_id] = std::move(inb);
    return resume_pos;
}

bool ResumableFileEngine::OnIncomingChunk(const PayloadFileChunkHeader& hdr,
                                          const uint8_t* data,
                                          bool& out_completed,
                                          bool& out_sha256_ok) {
    out_completed = false;
    out_sha256_ok = false;

    std::lock_guard<std::mutex> lk(mu_);
    auto it = inbound_.find(hdr.file_id);
    if (it == inbound_.end()) return false;
    auto& inb = it->second;

    if (hdr.offset + hdr.chunk_len > inb.total_bytes) return false;
    std::memcpy(inb.buffer.data() + hdr.offset, data, hdr.chunk_len);
    if (hdr.offset + hdr.chunk_len > inb.received_bytes) {
        inb.received_bytes = hdr.offset + hdr.chunk_len;
    }

    if (inb.received_bytes >= inb.total_bytes) {
        out_completed = true;
        auto actual_sha = CryptoEngine::Sha256(inb.buffer.data(), inb.buffer.size());
        out_sha256_ok = (actual_sha == inb.expected_sha256);
        if (out_sha256_ok) {
            std::ofstream ofs(inb.final_path, std::ios::binary | std::ios::trunc);
            if (!ofs) return false;
            if (!inb.buffer.empty()) {
                ofs.write(reinterpret_cast<const char*>(inb.buffer.data()),
                          static_cast<std::streamsize>(inb.buffer.size()));
            }
            std::remove((inb.final_path + ".uupart").c_str());
        }
    }
    return true;
}

std::vector<uint8_t> ResumableFileEngine::PackClipboardText(const std::string& utf8_text) {
    std::vector<uint8_t> out;
    uint32_t orig_len = static_cast<uint32_t>(utf8_text.size());
    out.resize(5);
    std::memcpy(out.data() + 1, &orig_len, 4);

    std::vector<uint8_t> lz4;
    size_t clen = CompressFastLz4(
        reinterpret_cast<const uint8_t*>(utf8_text.data()), utf8_text.size(), lz4);
    if (clen > 0) {
        out[0] = 1; // LZ4 compressed
        out.insert(out.end(), lz4.begin(), lz4.end());
    } else {
        out[0] = 0; // Raw UTF-8
        out.insert(out.end(), utf8_text.begin(), utf8_text.end());
    }
    return out;
}

std::string ResumableFileEngine::UnpackClipboardText(const uint8_t* data, size_t len) {
    if (!data || len < 5) return {};
    uint8_t flag = data[0];
    uint32_t orig_len = 0;
    std::memcpy(&orig_len, data + 1, 4);
    if (orig_len > 16 * 1024 * 1024) return {};

    if (flag == 1) {
        std::string s(orig_len, '\0');
        if (!DecompressFastLz4(data + 5, len - 5, reinterpret_cast<uint8_t*>(&s[0]), orig_len)) {
            return {};
        }
        return s;
    }
    if (len - 5 < orig_len) return {};
    return std::string(reinterpret_cast<const char*>(data + 5), orig_len);
}

} // namespace uu::core
