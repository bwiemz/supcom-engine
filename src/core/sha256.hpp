#pragma once

// SHA-256 (FIPS 180-4) and HMAC-SHA256 (RFC 2104): a saved game's snapshot
// is signed with its installation's key, so only snapshots this
// installation wrote are restored (M208c).

#include "core/types.hpp"

#include <array>
#include <cstddef>
#include <string_view>

namespace osc::core {

using Sha256Digest = std::array<u8, 32>;

class Sha256 {
public:
    Sha256();
    void update(const void* data, size_t size);
    Sha256Digest finish();

private:
    void block(const u8* p);
    std::array<u32, 8> h_;
    std::array<u8, 64> buffer_{};
    size_t buffered_ = 0;
    u64 length_ = 0; // bytes
};

Sha256Digest sha256(const void* data, size_t size);
inline Sha256Digest sha256(std::string_view s) {
    return sha256(s.data(), s.size());
}

/// HMAC-SHA256 of data given in parts.
class HmacSha256 {
public:
    HmacSha256(const void* key, size_t key_size);
    void update(const void* data, size_t size) { inner_.update(data, size); }
    Sha256Digest finish();

private:
    Sha256 inner_;
    std::array<u8, 64> opad_{};
};

Sha256Digest hmac_sha256(const void* key, size_t key_size, const void* data, size_t size);

/// Equal digests, compared in constant time.
bool digest_equal(const Sha256Digest& a, const Sha256Digest& b);

} // namespace osc::core
