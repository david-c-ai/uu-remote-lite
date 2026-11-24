#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace uu::core {

struct KeyPair25519 {
    std::array<uint8_t, 32> secret{};
    std::array<uint8_t, 32> pub{};
};

// Lightweight zero-OpenSSL cryptography suite (~15KB compiled code):
// - X25519 Diffie-Hellman key exchange (RFC 7748)
// - SHA-256 digest & HKDF-like session key derivation
// - ChaCha20-Poly1305 AEAD (RFC 8439) packet encryption & authentication
class CryptoEngine {
public:
    static KeyPair25519 GenerateKeyPair();
    static std::array<uint8_t, 32> X25519SharedSecret(
        const std::array<uint8_t, 32>& my_secret,
        const std::array<uint8_t, 32>& peer_pub);

    static std::array<uint8_t, 32> Sha256(const uint8_t* data, size_t len);

    static std::array<uint8_t, 32> DeriveSessionKey(
        const std::array<uint8_t, 32>& shared_secret,
        const char* access_pin);

    // Encrypts plaintext and appends 16-byte authentication tag; aad is authenticated in-place.
    static void SealPacket(
        const std::array<uint8_t, 32>& key,
        uint64_t nonce64,
        const uint8_t* aad, size_t aad_len,
        const uint8_t* plaintext, size_t pt_len,
        std::vector<uint8_t>& out_ciphertext_with_tag);

    // Verifies 16-byte authentication tag and decrypts ciphertext. Returns false if tampered!
    static bool OpenPacket(
        const std::array<uint8_t, 32>& key,
        uint64_t nonce64,
        const uint8_t* aad, size_t aad_len,
        const uint8_t* ciphertext_with_tag, size_t ct_tag_len,
        std::vector<uint8_t>& out_plaintext);
};

} // namespace uu::core
