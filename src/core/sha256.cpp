#include "core/sha256.hpp"

#include <algorithm>
#include <cstring>

namespace osc::core {

namespace {

constexpr std::array<u32, 64> kRound = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

constexpr u32 rotr(u32 x, int n) {
    return (x >> n) | (x << (32 - n));
}

} // namespace

Sha256::Sha256()
    : h_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
         0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19} {}

void Sha256::block(const u8* p) {
    std::array<u32, 64> w{};
    for (int i = 0; i < 16; ++i)
        w[i] = (u32{p[4 * i]} << 24) | (u32{p[4 * i + 1]} << 16) | (u32{p[4 * i + 2]} << 8) |
               u32{p[4 * i + 3]};
    for (int i = 16; i < 64; ++i) {
        const u32 s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const u32 s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    u32 a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
    for (int i = 0; i < 64; ++i) {
        const u32 s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const u32 ch = (e & f) ^ (~e & g);
        const u32 t1 = h + s1 + ch + kRound[i] + w[i];
        const u32 s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const u32 maj = (a & b) ^ (a & c) ^ (b & c);
        const u32 t2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h_[0] += a;
    h_[1] += b;
    h_[2] += c;
    h_[3] += d;
    h_[4] += e;
    h_[5] += f;
    h_[6] += g;
    h_[7] += h;
}

void Sha256::update(const void* data, size_t size) {
    const auto* p = static_cast<const u8*>(data);
    length_ += size;
    while (size > 0) {
        if (buffered_ == 0 && size >= 64) {
            block(p);
            p += 64;
            size -= 64;
            continue;
        }
        const size_t n = std::min(size, 64 - buffered_);
        std::memcpy(buffer_.data() + buffered_, p, n);
        buffered_ += n;
        p += n;
        size -= n;
        if (buffered_ == 64) {
            block(buffer_.data());
            buffered_ = 0;
        }
    }
}

Sha256Digest Sha256::finish() {
    const u64 bits = length_ * 8;
    const u8 one = 0x80;
    update(&one, 1);
    const u8 zero = 0;
    while (buffered_ != 56) update(&zero, 1);
    u8 len[8];
    for (int i = 0; i < 8; ++i) len[i] = static_cast<u8>(bits >> (56 - 8 * i));
    update(len, 8);
    Sha256Digest out{};
    for (int i = 0; i < 8; ++i)
        for (int k = 0; k < 4; ++k) out[4 * i + k] = static_cast<u8>(h_[i] >> (24 - 8 * k));
    return out;
}

Sha256Digest sha256(const void* data, size_t size) {
    Sha256 s;
    s.update(data, size);
    return s.finish();
}

HmacSha256::HmacSha256(const void* key, size_t key_size) {
    std::array<u8, 64> k{};
    if (key_size > k.size()) {
        const Sha256Digest d = sha256(key, key_size);
        std::memcpy(k.data(), d.data(), d.size());
    } else if (key_size > 0) {
        std::memcpy(k.data(), key, key_size);
    }
    std::array<u8, 64> ipad{};
    for (size_t i = 0; i < k.size(); ++i) {
        ipad[i] = static_cast<u8>(k[i] ^ 0x36);
        opad_[i] = static_cast<u8>(k[i] ^ 0x5c);
    }
    inner_.update(ipad.data(), ipad.size());
}

Sha256Digest HmacSha256::finish() {
    const Sha256Digest inner_digest = inner_.finish();
    Sha256 outer;
    outer.update(opad_.data(), opad_.size());
    outer.update(inner_digest.data(), inner_digest.size());
    return outer.finish();
}

Sha256Digest hmac_sha256(const void* key, size_t key_size, const void* data, size_t size) {
    HmacSha256 mac(key, key_size);
    mac.update(data, size);
    return mac.finish();
}

bool digest_equal(const Sha256Digest& a, const Sha256Digest& b) {
    u8 diff = 0;
    for (size_t i = 0; i < a.size(); ++i) diff = static_cast<u8>(diff | (a[i] ^ b[i]));
    return diff == 0;
}

} // namespace osc::core
