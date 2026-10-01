// SHA-256 and HMAC-SHA256 (M208c's snapshot signatures), against FIPS
// 180-4's and RFC 4231's test vectors.

#include <catch2/catch_test_macros.hpp>

#include "core/sha256.hpp"

#include <string>
#include <vector>

namespace {

std::string hex(const osc::core::Sha256Digest& d) {
    static const char* digits = "0123456789abcdef";
    std::string s;
    for (osc::u8 b : d) {
        s += digits[b >> 4];
        s += digits[b & 15];
    }
    return s;
}

std::string hmac(const std::string& key, const std::string& data) {
    return hex(osc::core::hmac_sha256(key.data(), key.size(), data.data(), data.size()));
}

} // namespace

TEST_CASE("SHA-256 gives FIPS 180-4's digests", "[sha256]") {
    CHECK(hex(osc::core::sha256("")) ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(hex(osc::core::sha256("abc")) ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(hex(osc::core::sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")) ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    // A million a's, fed in uneven pieces
    osc::core::Sha256 s;
    const std::string piece(997, 'a');
    size_t left = 1000000;
    while (left > 0) {
        const size_t n = std::min(left, piece.size());
        s.update(piece.data(), n);
        left -= n;
    }
    CHECK(hex(s.finish()) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("HMAC-SHA256 gives RFC 4231's", "[sha256]") {
    CHECK(hmac(std::string(20, '\x0b'), "Hi There") ==
          "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    CHECK(hmac("Jefe", "what do ya want for nothing?") ==
          "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    CHECK(
        hmac(std::string(131, '\xaa'), "Test Using Larger Than Block-Size Key - Hash Key First") ==
        "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
}

TEST_CASE("HMAC-SHA256 over parts is the HMAC of them joined", "[sha256]") {
    const std::string key = "Jefe";
    osc::core::HmacSha256 mac(key.data(), key.size());
    mac.update("what do ya ", 11);
    mac.update("", 0);
    mac.update("want for nothing?", 17);
    CHECK(hex(mac.finish()) == hmac(key, "what do ya want for nothing?"));
}

TEST_CASE("Digests compare equal only when they are", "[sha256]") {
    const auto a = osc::core::sha256("a");
    auto b = a;
    CHECK(osc::core::digest_equal(a, b));
    b[31] ^= 1;
    CHECK_FALSE(osc::core::digest_equal(a, b));
}
