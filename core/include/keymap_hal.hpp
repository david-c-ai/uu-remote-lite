#pragma once

#include <cstdint>

namespace uu::core {

// Canonical Platform-Independent Keycodes (aligned with USB HID Usage Page 0x07)
namespace DirKey {
constexpr uint16_t kNone      = 0x00;
constexpr uint16_t kA         = 0x04; // A..Z = 0x04..0x1D
constexpr uint16_t kZ         = 0x1D;
constexpr uint16_t k1         = 0x1E; // 1..9 = 0x1E..0x26
constexpr uint16_t k0         = 0x27;
constexpr uint16_t kEnter     = 0x28;
constexpr uint16_t kEscape    = 0x29;
constexpr uint16_t kBackspace = 0x2A;
constexpr uint16_t kTab       = 0x2B;
constexpr uint16_t kSpace     = 0x2C;
constexpr uint16_t kMinus     = 0x2D;
constexpr uint16_t kEqual     = 0x2E;
constexpr uint16_t kLBracket  = 0x2F;
constexpr uint16_t kRBracket  = 0x30;
constexpr uint16_t kBackslash = 0x31;
constexpr uint16_t kSemicolon = 0x33;
constexpr uint16_t kQuote     = 0x34;
constexpr uint16_t kGrave     = 0x35;
constexpr uint16_t kComma     = 0x36;
constexpr uint16_t kPeriod    = 0x37;
constexpr uint16_t kSlash     = 0x38;
constexpr uint16_t kCapsLock  = 0x39;
constexpr uint16_t kF1        = 0x3A; // F1..F12 = 0x3A..0x45
constexpr uint16_t kF12       = 0x45;
constexpr uint16_t kDelete    = 0x4C;
constexpr uint16_t kRight     = 0x4F;
constexpr uint16_t kLeft      = 0x50;
constexpr uint16_t kDown      = 0x51;
constexpr uint16_t kUp        = 0x52;
constexpr uint16_t kHome      = 0x4A;
constexpr uint16_t kEnd       = 0x4D;
constexpr uint16_t kPageUp    = 0x4B;
constexpr uint16_t kPageDown  = 0x4E;
constexpr uint16_t kCtrl      = 0xE0;
constexpr uint16_t kShift     = 0xE1;
constexpr uint16_t kAlt       = 0xE2;
constexpr uint16_t kMeta      = 0xE3;
} // namespace DirKey

// Modifier bitmask carried in PayloadKeyInput::modifiers
constexpr uint8_t kModShift = 0x01;
constexpr uint8_t kModCtrl  = 0x02;
constexpr uint8_t kModAlt   = 0x04;
constexpr uint8_t kModMeta  = 0x08; // Cmd on macOS, Win key on Windows
constexpr uint8_t kModSwapCmdCtrl = 0x80; // Auto-swap Cmd <-> Ctrl across Win <-> Mac!

enum class HostOsType : uint8_t {
    kUnknown = 0,
    kMacOS   = 1,
    kWindows = 2,
    kLinux   = 3,
    kAndroid = 4,
    kIOS     = 5,
};

HostOsType LocalOsType();

// Remaps modifiers when controlling across different desktop OSes (Mac <-> Windows):
// e.g. Mac Cmd+C -> Windows Ctrl+C, Windows Ctrl+V -> Mac Cmd+V
uint8_t RemapModifiersCrossOs(uint8_t modifiers, HostOsType client_os, HostOsType host_os);

// Canonical DirKey -> macOS Carbon CGKeyCode (returns 0xFFFF if unmapped)
uint16_t DirKeyToMacVirtualKey(uint16_t dir_key);

// Canonical DirKey -> Windows Win32 VK_* code (returns 0 if unmapped)
uint16_t DirKeyToWinVirtualKey(uint16_t dir_key);

} // namespace uu::core
