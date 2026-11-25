#include "keymap_hal.hpp"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace uu::core {

HostOsType LocalOsType() {
#if defined(__APPLE__)
#if TARGET_OS_OSX
    return HostOsType::kMacOS;
#else
    return HostOsType::kIOS;
#endif
#elif defined(_WIN32)
    return HostOsType::kWindows;
#elif defined(__ANDROID__)
    return HostOsType::kAndroid;
#else
    return HostOsType::kLinux;
#endif
}

uint8_t RemapModifiersCrossOs(uint8_t modifiers, HostOsType client_os, HostOsType host_os) {
    bool explicit_swap = (modifiers & kModSwapCmdCtrl) != 0;
    uint8_t clean = modifiers & 0x0F;

    bool mac_win_pair =
        (client_os == HostOsType::kMacOS && host_os == HostOsType::kWindows) ||
        (client_os == HostOsType::kWindows && host_os == HostOsType::kMacOS);

    if (explicit_swap || mac_win_pair) {
        bool had_ctrl = (clean & kModCtrl) != 0;
        bool had_meta = (clean & kModMeta) != 0;
        clean &= static_cast<uint8_t>(~(kModCtrl | kModMeta));
        if (had_ctrl) clean |= kModMeta;
        if (had_meta) clean |= kModCtrl;
    }
    return clean;
}

uint16_t DirKeyToMacVirtualKey(uint16_t k) {
    // Standard Apple ANSI kVK_* table
    switch (k) {
        case 0x04: return 0x00; // A
        case 0x05: return 0x0B; // B
        case 0x06: return 0x08; // C
        case 0x07: return 0x02; // D
        case 0x08: return 0x0E; // E
        case 0x09: return 0x03; // F
        case 0x0A: return 0x05; // G
        case 0x0B: return 0x04; // H
        case 0x0C: return 0x22; // I
        case 0x0D: return 0x26; // J
        case 0x0E: return 0x28; // K
        case 0x0F: return 0x25; // L
        case 0x10: return 0x2E; // M
        case 0x11: return 0x2D; // N
        case 0x12: return 0x1F; // O
        case 0x13: return 0x23; // P
        case 0x14: return 0x0C; // Q
        case 0x15: return 0x0F; // R
        case 0x16: return 0x01; // S
        case 0x17: return 0x11; // T
        case 0x18: return 0x20; // U
        case 0x19: return 0x09; // V
        case 0x1A: return 0x0D; // W
        case 0x1B: return 0x07; // X
        case 0x1C: return 0x10; // Y
        case 0x1D: return 0x06; // Z
        case 0x1E: return 0x12; // 1
        case 0x1F: return 0x13; // 2
        case 0x20: return 0x14; // 3
        case 0x21: return 0x15; // 4
        case 0x22: return 0x17; // 5
        case 0x23: return 0x16; // 6
        case 0x24: return 0x1A; // 7
        case 0x25: return 0x1C; // 8
        case 0x26: return 0x19; // 9
        case 0x27: return 0x1D; // 0
        case DirKey::kEnter:     return 0x24;
        case DirKey::kEscape:    return 0x35;
        case DirKey::kBackspace: return 0x33;
        case DirKey::kTab:       return 0x30;
        case DirKey::kSpace:     return 0x31;
        case DirKey::kMinus:     return 0x1B;
        case DirKey::kEqual:     return 0x18;
        case DirKey::kLBracket:  return 0x21;
        case DirKey::kRBracket:  return 0x1E;
        case DirKey::kBackslash: return 0x2A;
        case DirKey::kSemicolon: return 0x29;
        case DirKey::kQuote:     return 0x27;
        case DirKey::kGrave:     return 0x32;
        case DirKey::kComma:     return 0x2B;
        case DirKey::kPeriod:    return 0x2F;
        case DirKey::kSlash:     return 0x2C;
        case DirKey::kCapsLock:  return 0x39;
        case 0x3A: return 0x7A; // F1
        case 0x3B: return 0x78; // F2
        case 0x3C: return 0x63; // F3
        case 0x3D: return 0x76; // F4
        case 0x3E: return 0x60; // F5
        case 0x3F: return 0x61; // F6
        case 0x40: return 0x62; // F7
        case 0x41: return 0x64; // F8
        case 0x42: return 0x65; // F9
        case 0x43: return 0x6D; // F10
        case 0x44: return 0x67; // F11
        case 0x45: return 0x6F; // F12
        case DirKey::kDelete:   return 0x75;
        case DirKey::kHome:     return 0x73;
        case DirKey::kEnd:      return 0x77;
        case DirKey::kPageUp:   return 0x74;
        case DirKey::kPageDown: return 0x79;
        case DirKey::kRight:    return 0x7C;
        case DirKey::kLeft:     return 0x7B;
        case DirKey::kDown:     return 0x7D;
        case DirKey::kUp:       return 0x7E;
        case DirKey::kCtrl:     return 0x3B;
        case DirKey::kShift:    return 0x38;
        case DirKey::kAlt:      return 0x3A;
        case DirKey::kMeta:     return 0x37;
        default: return 0xFFFF;
    }
}

uint16_t DirKeyToWinVirtualKey(uint16_t k) {
    if (k >= DirKey::kA && k <= DirKey::kZ) {
        return static_cast<uint16_t>('A' + (k - DirKey::kA));
    }
    if (k >= DirKey::k1 && k <= 0x26) {
        return static_cast<uint16_t>('1' + (k - DirKey::k1));
    }
    if (k == DirKey::k0) return '0';
    if (k >= DirKey::kF1 && k <= DirKey::kF12) {
        return static_cast<uint16_t>(0x70 + (k - DirKey::kF1)); // VK_F1..VK_F12
    }
    switch (k) {
        case DirKey::kEnter:     return 0x0D; // VK_RETURN
        case DirKey::kEscape:    return 0x1B; // VK_ESCAPE
        case DirKey::kBackspace: return 0x08; // VK_BACK
        case DirKey::kTab:       return 0x09; // VK_TAB
        case DirKey::kSpace:     return 0x20; // VK_SPACE
        case DirKey::kMinus:     return 0xBD; // VK_OEM_MINUS
        case DirKey::kEqual:     return 0xBB; // VK_OEM_PLUS
        case DirKey::kLBracket:  return 0xDB; // VK_OEM_4
        case DirKey::kRBracket:  return 0xDD; // VK_OEM_6
        case DirKey::kBackslash: return 0xDC; // VK_OEM_5
        case DirKey::kSemicolon: return 0xBA; // VK_OEM_1
        case DirKey::kQuote:     return 0xDE; // VK_OEM_7
        case DirKey::kGrave:     return 0xC0; // VK_OEM_3
        case DirKey::kComma:     return 0xBC; // VK_OEM_COMMA
        case DirKey::kPeriod:    return 0xBE; // VK_OEM_PERIOD
        case DirKey::kSlash:     return 0xBF; // VK_OEM_2
        case DirKey::kCapsLock:  return 0x14; // VK_CAPITAL
        case DirKey::kDelete:    return 0x2E; // VK_DELETE
        case DirKey::kHome:      return 0x24; // VK_HOME
        case DirKey::kEnd:       return 0x23; // VK_END
        case DirKey::kPageUp:    return 0x21; // VK_PRIOR
        case DirKey::kPageDown:  return 0x22; // VK_NEXT
        case DirKey::kRight:     return 0x27; // VK_RIGHT
        case DirKey::kLeft:      return 0x25; // VK_LEFT
        case DirKey::kDown:      return 0x28; // VK_DOWN
        case DirKey::kUp:        return 0x26; // VK_UP
        case DirKey::kCtrl:      return 0x11; // VK_CONTROL
        case DirKey::kShift:     return 0x10; // VK_SHIFT
        case DirKey::kAlt:       return 0x12; // VK_MENU
        case DirKey::kMeta:      return 0x5B; // VK_LWIN
        default: return 0;
    }
}

} // namespace uu::core
