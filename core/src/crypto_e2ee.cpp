#include "crypto_e2ee.hpp"

#include <cstring>
#include <random>

namespace uu::core {

namespace {

inline uint32_t RotL32(uint32_t x, int n) {
    return (x << n) | (x >> (32 - n));
}

inline uint32_t RotR32(uint32_t x, int n) {
    return (x >> n) | (x << (32 - n));
}

inline uint32_t LoadLE32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

inline void StoreLE32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

// --- RFC 8439 ChaCha20 Block Function ---
void ChaCha20QuarterRound(uint32_t& a, uint32_t& b, uint32_t& c, uint32_t& d) {
    a += b; d ^= a; d = RotL32(d, 16);
    c += d; b ^= c; b = RotL32(b, 12);
    a += b; d ^= a; d = RotL32(d, 8);
    c += d; b ^= c; b = RotL32(b, 7);
}

void ChaCha20Block(const std::array<uint8_t, 32>& key, uint32_t counter,
                   uint64_t nonce64, uint8_t out_block[64]) {
    uint32_t st[16] = {
        0x61707865, 0x3320646e, 0x79622d32, 0x6b206574,
        LoadLE32(&key[0]),  LoadLE32(&key[4]),  LoadLE32(&key[8]),  LoadLE32(&key[12]),
        LoadLE32(&key[16]), LoadLE32(&key[20]), LoadLE32(&key[24]), LoadLE32(&key[28]),
        counter, 0,
        static_cast<uint32_t>(nonce64 & 0xFFFFFFFFULL),
        static_cast<uint32_t>(nonce64 >> 32)
    };
    uint32_t w[16];
    std::memcpy(w, st, sizeof(st));
    for (int i = 0; i < 10; ++i) {
        ChaCha20QuarterRound(w[0], w[4], w[8],  w[12]);
        ChaCha20QuarterRound(w[1], w[5], w[9],  w[13]);
        ChaCha20QuarterRound(w[2], w[6], w[10], w[14]);
        ChaCha20QuarterRound(w[3], w[7], w[11], w[15]);
        ChaCha20QuarterRound(w[0], w[5], w[10], w[15]);
        ChaCha20QuarterRound(w[1], w[6], w[11], w[12]);
        ChaCha20QuarterRound(w[2], w[7], w[8],  w[13]);
        ChaCha20QuarterRound(w[3], w[4], w[9],  w[14]);
    }
    for (int i = 0; i < 16; ++i) {
        StoreLE32(out_block + i * 4, w[i] + st[i]);
    }
}

void ChaCha20Xor(const std::array<uint8_t, 32>& key, uint32_t init_counter,
                 uint64_t nonce64, const uint8_t* in, uint8_t* out, size_t len) {
    uint8_t block[64];
    uint32_t ctr = init_counter;
    size_t offset = 0;
    while (offset < len) {
        ChaCha20Block(key, ctr++, nonce64, block);
        size_t chunk = (len - offset < 64) ? (len - offset) : 64;
        for (size_t i = 0; i < chunk; ++i) {
            out[offset + i] = in[offset + i] ^ block[i];
        }
        offset += chunk;
    }
}

// --- Portable FIPS 180-4 SHA-256 ---
struct Sha256State {
    uint32_t h[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };
    uint8_t buf[64]{};
    size_t buf_len = 0;
    uint64_t total_bits = 0;

    void Compress(const uint8_t block[64]) {
        static const uint32_t k[64] = {
            0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
            0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
            0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
            0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
            0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
            0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
            0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
            0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
        };
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) |
                   (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
                   static_cast<uint32_t>(block[i * 4 + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = RotR32(w[i - 15], 7) ^ RotR32(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = RotR32(w[i - 2], 17) ^ RotR32(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t S1 = RotR32(e, 6) ^ RotR32(e, 11) ^ RotR32(e, 25);
            uint32_t ch = (e & f) ^ ((~e) & g);
            uint32_t temp1 = hh + S1 + ch + k[i] + w[i];
            uint32_t S0 = RotR32(a, 2) ^ RotR32(a, 13) ^ RotR32(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t temp2 = S0 + maj;
            hh = g; g = f; f = e; e = d + temp1;
            d = c; c = b; b = a; a = temp1 + temp2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }

    void Update(const uint8_t* data, size_t len) {
        total_bits += static_cast<uint64_t>(len) * 8;
        size_t i = 0;
        while (i < len) {
            size_t take = (64 - buf_len < len - i) ? (64 - buf_len) : (len - i);
            std::memcpy(buf + buf_len, data + i, take);
            buf_len += take;
            i += take;
            if (buf_len == 64) {
                Compress(buf);
                buf_len = 0;
            }
        }
    }

    std::array<uint8_t, 32> Finalize() {
        buf[buf_len++] = 0x80;
        if (buf_len > 56) {
            std::memset(buf + buf_len, 0, 64 - buf_len);
            Compress(buf);
            buf_len = 0;
        }
        std::memset(buf + buf_len, 0, 56 - buf_len);
        for (int i = 0; i < 8; ++i) {
            buf[56 + i] = static_cast<uint8_t>((total_bits >> (56 - i * 8)) & 0xFF);
        }
        Compress(buf);
        std::array<uint8_t, 32> out{};
        for (int i = 0; i < 8; ++i) {
            out[i * 4 + 0] = static_cast<uint8_t>((h[i] >> 24) & 0xFF);
            out[i * 4 + 1] = static_cast<uint8_t>((h[i] >> 16) & 0xFF);
            out[i * 4 + 2] = static_cast<uint8_t>((h[i] >> 8) & 0xFF);
            out[i * 4 + 3] = static_cast<uint8_t>(h[i] & 0xFF);
        }
        return out;
    }
};

// --- Compact RFC 7748 X25519 Curve25519 Field Arithmetic (TweetNaCl / Ref10 style 16-limb) ---
using Fe25519 = int64_t[16];

void FeCarry(Fe25519 o) {
    for (int i = 0; i < 16; ++i) {
        o[i] += (1LL << 16);
        int64_t c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c << 16;
    }
}

void FeAdd(Fe25519 o, const Fe25519 a, const Fe25519 b) {
    for (int i = 0; i < 16; ++i) o[i] = a[i] + b[i];
}

void FeSub(Fe25519 o, const Fe25519 a, const Fe25519 b) {
    for (int i = 0; i < 16; ++i) o[i] = a[i] - b[i];
}

void FeMul(Fe25519 o, const Fe25519 a, const Fe25519 b) {
    int64_t t[31]{};
    for (int i = 0; i < 16; ++i) {
        for (int j = 0; j < 16; ++j) {
            t[i + j] += a[i] * b[j];
        }
    }
    for (int i = 0; i < 15; ++i) {
        t[i] += 38 * t[i + 16];
    }
    for (int i = 0; i < 16; ++i) o[i] = t[i];
    FeCarry(o);
    FeCarry(o);
}

void FeSwap(Fe25519 p, Fe25519 q, int b) {
    int64_t mask = ~(static_cast<int64_t>(b) - 1);
    for (int i = 0; i < 16; ++i) {
        int64_t t = mask & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

void FeInvert(Fe25519 o, const Fe25519 i) {
    Fe25519 c;
    std::memcpy(c, i, sizeof(Fe25519));
    for (int a = 253; a >= 0; --a) {
        FeMul(c, c, c);
        if (a != 2 && a != 4) FeMul(c, c, i);
    }
    std::memcpy(o, c, sizeof(Fe25519));
}

void ScalarMultX25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]) {
    uint8_t z[32];
    std::memcpy(z, scalar, 32);
    z[0] &= 248;
    z[31] = (z[31] & 127) | 64;

    Fe25519 x, a{}, b{}, c{}, d{}, e{}, f{};
    for (int i = 0; i < 16; ++i) {
        x[i] = static_cast<int64_t>(point[2 * i]) + (static_cast<int64_t>(point[2 * i + 1]) << 8);
    }
    x[15] &= 0x7fff;
    a[0] = d[0] = 1;
    std::memcpy(b, x, sizeof(Fe25519));

    static const Fe25519 k121665 = {0xDB41, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

    for (int i = 254; i >= 0; --i) {
        int bit = (z[i >> 3] >> (i & 7)) & 1;
        FeSwap(a, b, bit);
        FeSwap(c, d, bit);
        FeAdd(e, a, c);
        FeSub(a, a, c);
        FeAdd(c, b, d);
        FeSub(b, b, d);
        FeMul(d, e, e);
        FeMul(f, a, a);
        FeMul(a, c, a);
        FeMul(c, b, e);
        FeAdd(e, a, c);
        FeSub(a, a, c);
        FeMul(b, a, a);
        FeSub(c, d, f);
        FeMul(a, c, k121665);
        FeAdd(a, a, d);
        FeMul(c, c, a);
        FeMul(a, d, f);
        FeMul(d, b, x);
        FeMul(b, e, e);
        FeSwap(a, b, bit);
        FeSwap(c, d, bit);
    }

    FeInvert(c, c);
    FeMul(a, a, c);
    FeCarry(a);
    FeCarry(a);
    FeCarry(a);

    // Final canonical mod 2^255 - 19 reduction
    Fe25519 m;
    for (int j = 0; j < 2; ++j) {
        m[0] = a[0] - 0xffed;
        for (int i = 1; i < 15; ++i) {
            m[i] = a[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = a[15] - 0x7fff - ((m[14] >> 16) & 1);
        int swap_b = static_cast<int>(1 - ((m[15] >> 16) & 1));
        m[14] &= 0xffff;
        FeSwap(a, m, swap_b);
    }

    for (int i = 0; i < 16; ++i) {
        out[2 * i] = static_cast<uint8_t>(a[i] & 0xff);
        out[2 * i + 1] = static_cast<uint8_t>((a[i] >> 8) & 0xff);
    }
}

// Compute 16-byte AEAD authentication tag using subkey from ChaCha20 block 0 + keyed SHA-256 MAC
void ComputeAeadTag16(const std::array<uint8_t, 32>& key, uint64_t nonce64,
                      const uint8_t* aad, size_t aad_len,
                      const uint8_t* ct, size_t ct_len,
                      uint8_t out_tag[16]) {
    uint8_t block0[64];
    ChaCha20Block(key, 0, nonce64, block0);
    Sha256State st;
    st.Update(block0, 32);
    uint64_t lens[2] = {aad_len, ct_len};
    st.Update(reinterpret_cast<const uint8_t*>(lens), sizeof(lens));
    if (aad && aad_len > 0) st.Update(aad, aad_len);
    if (ct && ct_len > 0) st.Update(ct, ct_len);
    st.Update(block0 + 32, 32);
    auto digest = st.Finalize();
    std::memcpy(out_tag, digest.data(), 16);
}

} // namespace

KeyPair25519 CryptoEngine::GenerateKeyPair() {
    KeyPair25519 kp{};
    std::random_device rd;
    for (size_t i = 0; i < 32; i += 4) {
        uint32_t r = rd();
        StoreLE32(&kp.secret[i], r);
    }
    kp.secret[0] &= 248;
    kp.secret[31] = (kp.secret[31] & 127) | 64;

    uint8_t basepoint[32]{9};
    ScalarMultX25519(kp.pub.data(), kp.secret.data(), basepoint);
    return kp;
}

std::array<uint8_t, 32> CryptoEngine::X25519SharedSecret(
    const std::array<uint8_t, 32>& my_secret,
    const std::array<uint8_t, 32>& peer_pub) {
    std::array<uint8_t, 32> shared{};
    ScalarMultX25519(shared.data(), my_secret.data(), peer_pub.data());
    return shared;
}

std::array<uint8_t, 32> CryptoEngine::Sha256(const uint8_t* data, size_t len) {
    Sha256State st;
    if (data && len > 0) st.Update(data, len);
    return st.Finalize();
}

std::array<uint8_t, 32> CryptoEngine::DeriveSessionKey(
    const std::array<uint8_t, 32>& shared_secret,
    const char* access_pin) {
    Sha256State st;
    const char domain[] = "UURemoteLite-E2EE-v2";
    st.Update(reinterpret_cast<const uint8_t*>(domain), sizeof(domain) - 1);
    st.Update(shared_secret.data(), shared_secret.size());
    if (access_pin && *access_pin) {
        st.Update(reinterpret_cast<const uint8_t*>(access_pin), std::strlen(access_pin));
    }
    return st.Finalize();
}

void CryptoEngine::SealPacket(
    const std::array<uint8_t, 32>& key,
    uint64_t nonce64,
    const uint8_t* aad, size_t aad_len,
    const uint8_t* plaintext, size_t pt_len,
    std::vector<uint8_t>& out_ciphertext_with_tag) {
    out_ciphertext_with_tag.resize(pt_len + 16);
    if (pt_len > 0) {
        ChaCha20Xor(key, 1, nonce64, plaintext, out_ciphertext_with_tag.data(), pt_len);
    }
    ComputeAeadTag16(key, nonce64, aad, aad_len,
                     out_ciphertext_with_tag.data(), pt_len,
                     out_ciphertext_with_tag.data() + pt_len);
}

bool CryptoEngine::OpenPacket(
    const std::array<uint8_t, 32>& key,
    uint64_t nonce64,
    const uint8_t* aad, size_t aad_len,
    const uint8_t* ciphertext_with_tag, size_t ct_tag_len,
    std::vector<uint8_t>& out_plaintext) {
    if (ct_tag_len < 16) return false;
    size_t ct_len = ct_tag_len - 16;

    uint8_t expected_tag[16];
    ComputeAeadTag16(key, nonce64, aad, aad_len, ciphertext_with_tag, ct_len, expected_tag);

    // Constant-time 16-byte tag verification
    uint8_t diff = 0;
    for (size_t i = 0; i < 16; ++i) {
        diff |= (expected_tag[i] ^ ciphertext_with_tag[ct_len + i]);
    }
    if (diff != 0) return false;

    out_plaintext.resize(ct_len);
    if (ct_len > 0) {
        ChaCha20Xor(key, 1, nonce64, ciphertext_with_tag, out_plaintext.data(), ct_len);
    }
    return true;
}

} // namespace uu::core
