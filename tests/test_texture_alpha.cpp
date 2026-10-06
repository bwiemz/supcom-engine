// A texture's alpha for hit tests (Bitmap:UseAlphaHitTest): the mask is
// read from the DDS file's top mip, and a file that declares more texels
// than it holds is refused before any mask is made.

#include <catch2/catch_test_macros.hpp>

#include "ui/texture_alpha.hpp"

#include <cstring>
#include <vector>

namespace {

using osc::u32;
using osc::ui::AlphaMask;

constexpr u32 kFourCC = 0x4;
constexpr u32 kRgb = 0x40;
constexpr u32 kAlphaPixels = 0x1;

/// A DDS header (128 bytes) for `width` x `height`, then `payload` bytes.
std::vector<char> dds(u32 width, u32 height, u32 flags, u32 fourcc, size_t payload, u32 bits = 0,
                      u32 alpha_mask = 0) {
    std::vector<char> f(128 + payload, 0);
    const auto put = [&](size_t at, u32 v) { std::memcpy(f.data() + at, &v, 4); };
    put(0, 0x20534444); // "DDS "
    put(4, 124);
    put(12, height);
    put(16, width);
    put(80, flags);
    put(84, fourcc);
    put(88, bits);
    put(104, alpha_mask);
    return f;
}

constexpr u32 kDxt1 = 0x31545844;
constexpr u32 kDxt3 = 0x33545844;
constexpr u32 kDxt5 = 0x35545844;

} // namespace

TEST_CASE("A DDS that declares more texels than it holds is refused", "[ui][texture]") {
    // The review's case: 128 bytes declaring 65536 x 65536 (it asked for
    // 512 MiB before it was refused)
    CHECK_FALSE(AlphaMask::from_dds(dds(65536, 65536, kFourCC, kDxt5, 0)));
    CHECK_FALSE(AlphaMask::from_dds(dds(16384, 16384, kFourCC, kDxt1, 0)));
    CHECK_FALSE(AlphaMask::from_dds(dds(16384, 16384, kRgb, 0, 64, 32)));
    // One byte short of each format's payload
    CHECK_FALSE(AlphaMask::from_dds(dds(8, 8, kFourCC, kDxt1, 4 * 8 - 1)));
    CHECK_FALSE(AlphaMask::from_dds(dds(8, 8, kFourCC, kDxt3, 4 * 16 - 1)));
    CHECK_FALSE(AlphaMask::from_dds(dds(5, 5, kFourCC, kDxt5, 4 * 16 - 1))); // 2 x 2 blocks
    CHECK_FALSE(AlphaMask::from_dds(dds(3, 3, kRgb, 0, 3 * 3 * 4 - 1, 32)));
    // Exactly enough is read
    CHECK(AlphaMask::from_dds(dds(8, 8, kFourCC, kDxt1, 4 * 8)));
    CHECK(AlphaMask::from_dds(dds(5, 5, kFourCC, kDxt5, 4 * 16)));
    CHECK(AlphaMask::from_dds(dds(3, 3, kRgb, 0, 3 * 3 * 4, 32)));
}

TEST_CASE("A malformed or oversized DDS gives no mask", "[ui][texture]") {
    CHECK_FALSE(AlphaMask::from_dds({}));
    CHECK_FALSE(AlphaMask::from_dds(std::vector<char>(127, 0))); // short of a header
    auto bad_magic = dds(4, 4, kFourCC, kDxt1, 8);
    bad_magic[0] = 'X';
    CHECK_FALSE(AlphaMask::from_dds(bad_magic));
    CHECK_FALSE(AlphaMask::from_dds(dds(0, 4, kFourCC, kDxt1, 8)));
    CHECK_FALSE(AlphaMask::from_dds(dds(4, 0, kFourCC, kDxt1, 8)));
    CHECK_FALSE(AlphaMask::from_dds(dds(4, 4, kFourCC, 0x31495441 /* ATI1 */, 8)));
    CHECK_FALSE(AlphaMask::from_dds(dds(4, 4, kRgb, 0, 4 * 4 * 3, 24))); // not 32-bit
    CHECK_FALSE(AlphaMask::from_dds(dds(4, 4, 0, 0, 64)));               // neither
    // Past the largest side a mask is made for, even with the payload there
    CHECK_FALSE(AlphaMask::from_dds(dds(16385, 1, kRgb, 0, 16385 * 4, 32)));
}

TEST_CASE("A DDS's alpha mask reads its texels", "[ui][texture]") {
    // DXT5, 5 x 5 (2 x 2 blocks): the first block's alphas all 0, the
    // others 255 (a0 = a1 = 255 with zero indices)
    auto f = dds(5, 5, kFourCC, kDxt5, 4 * 16);
    for (int b = 1; b < 4; ++b) {
        f[128 + b * 16] = static_cast<char>(255);
        f[128 + b * 16 + 1] = static_cast<char>(255);
    }
    const auto mask = AlphaMask::from_dds(f);
    REQUIRE(mask);
    CHECK_FALSE(mask->opaque(0, 0));
    CHECK_FALSE(mask->opaque(3, 3));
    CHECK(mask->opaque(4, 0));
    CHECK(mask->opaque(4, 4));
    CHECK_FALSE(mask->opaque(5, 0)); // off the texture
    CHECK_FALSE(mask->opaque(-1, 0));

    // RGB32 with an alpha mask: the second texel's alpha is set
    auto rgb = dds(2, 1, kRgb | kAlphaPixels, 0, 8, 32, 0xFF000000u);
    rgb[128 + 4 + 3] = static_cast<char>(0x80);
    const auto rgb_mask = AlphaMask::from_dds(rgb);
    REQUIRE(rgb_mask);
    CHECK_FALSE(rgb_mask->opaque(0, 0));
    CHECK(rgb_mask->opaque(1, 0));

    // DXT3: a block's 4-bit alphas, the first texel's only
    auto dxt3 = dds(4, 4, kFourCC, kDxt3, 16);
    dxt3[128] = 0x0F;
    const auto dxt3_mask = AlphaMask::from_dds(dxt3);
    REQUIRE(dxt3_mask);
    CHECK(dxt3_mask->opaque(0, 0));
    CHECK_FALSE(dxt3_mask->opaque(1, 0));

    // DXT1 with c0 <= c1 and index 3: transparent
    auto dxt1 = dds(4, 4, kFourCC, kDxt1, 8);
    std::memset(dxt1.data() + 128 + 4, 0xFF, 4); // every index 3
    const auto dxt1_mask = AlphaMask::from_dds(dxt1);
    REQUIRE(dxt1_mask);
    CHECK_FALSE(dxt1_mask->opaque(2, 2));
}

// Its refusal comes before the mask is made: in a child whose address space
// allows 64 MiB more than it has, the 512 MiB a 65536 x 65536 mask needs
// would fail to allocate. (Linux; skipped under ASan, whose shadow memory a
// limit on the address space breaks. Skipped, not left out, so every Linux
// build lists the same test cases: docs/current-state.md counts them.)
#if defined(__linux__)
#if defined(__SANITIZE_ADDRESS__)
#define OSC_TEXTURE_ALPHA_NO_RLIMIT_TEST
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define OSC_TEXTURE_ALPHA_NO_RLIMIT_TEST
#endif
#endif
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <fstream>
#include <new>

TEST_CASE("A truncated DDS declaring a huge texture is refused before its mask is allocated",
          "[ui][texture]") {
#ifdef OSC_TEXTURE_ALPHA_NO_RLIMIT_TEST
    SKIP("a limit on the address space breaks ASan's shadow memory");
#else
    const auto file = dds(65536, 65536, kFourCC, kDxt5, 0);
    const pid_t pid = fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        long pages = 0;
        std::ifstream("/proc/self/statm") >> pages;
        const rlim_t have = static_cast<rlim_t>(pages) * static_cast<rlim_t>(sysconf(_SC_PAGESIZE));
        rlimit lim{};
        getrlimit(RLIMIT_AS, &lim);
        lim.rlim_cur = have + (rlim_t{64} << 20);
        if (setrlimit(RLIMIT_AS, &lim) != 0) _exit(3);
        try {
            _exit(AlphaMask::from_dds(file) ? 2 : 0);
        } catch (const std::bad_alloc&) {
            _exit(1); // it tried to allocate the mask
        }
    }
    int status = 0;
    REQUIRE(waitpid(pid, &status, 0) == pid);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
#endif
}
#endif
