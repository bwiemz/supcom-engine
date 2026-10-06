#include <catch2/catch_test_macros.hpp>

#include "renderer/dds_decode.hpp"
#include "renderer/dds_parser.hpp"

#include <cstring>
#include <vector>

using namespace osc;
using namespace osc::renderer;

namespace {

/// A 4x4 DXT1 DDS with two mips (a block each) on each of `faces` faces,
/// every block's first byte marking its face and mip (face * 16 + mip).
std::vector<char> dxt1(u32 faces, u32 caps2) {
    constexpr u32 kBlock = 8;
    std::vector<char> d(128 + static_cast<size_t>(faces) * 2 * kBlock, 0);
    const auto put = [&](size_t offset, u32 v) { std::memcpy(d.data() + offset, &v, 4); };
    std::memcpy(d.data(), "DDS ", 4);
    put(4, 124);
    put(8, 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000); // caps, height, width, pixel format, mips
    put(12, 4);
    put(16, 4);
    put(28, 2); // two mips
    put(76, 32);
    put(80, 0x4); // FourCC
    std::memcpy(d.data() + 84, "DXT1", 4);
    put(108, 0x1000 | 0x8 | 0x400000);
    put(112, caps2);
    for (u32 f = 0; f < faces; ++f)
        for (u32 m = 0; m < 2; ++m) d[128 + (f * 2 + m) * kBlock] = static_cast<char>(f * 16 + m);
    return d;
}

} // namespace

TEST_CASE("A cubemap DDS parses as six faces, each with its mips (M211a)", "[dds]") {
    const auto file = dxt1(6, 0x200 | 0xFC00);
    const auto tex = parse_dds(file);
    REQUIRE(tex);
    CHECK(tex->faces == 6);
    CHECK(tex->mip_count == 2);
    REQUIRE(tex->mips.size() == 12);
    for (u32 f = 0; f < 6; ++f) {
        for (u32 m = 0; m < 2; ++m) {
            const DDSMipLevel& mip = tex->mips[f * 2 + m];
            CHECK(static_cast<u32>(static_cast<unsigned char>(mip.data[0])) == f * 16 + m);
            CHECK(mip.width == (m == 0 ? 4u : 2u));
        }
    }
}

TEST_CASE("A 2D DDS is one face (M211a)", "[dds]") {
    const auto tex = parse_dds(dxt1(1, 0));
    REQUIRE(tex);
    CHECK(tex->faces == 1);
    CHECK(tex->mips.size() == 2);
}

TEST_CASE("A cubemap without all six faces, or cut short, is refused (M211a)", "[dds]") {
    CHECK_FALSE(parse_dds(dxt1(1, 0x200 | 0x400))); // +X alone
    auto cut = dxt1(6, 0x200 | 0xFC00);
    cut.resize(cut.size() - 8); // the last face's last mip gone
    CHECK_FALSE(parse_dds(cut));
}

TEST_CASE("An A8 DDS parses as alpha alone, one byte a texel (M210b)", "[dds]") {
    // retail's horizonLookup.dds: 128 x 4, DDPF_ALPHA, 8 bits
    std::vector<char> d(128 + 128 * 4, 0);
    const auto put = [&](size_t offset, u32 v) { std::memcpy(d.data() + offset, &v, 4); };
    std::memcpy(d.data(), "DDS ", 4);
    put(4, 124);
    put(12, 4);   // height
    put(16, 128); // width
    put(76, 32);
    put(80, 0x2); // DDPF_ALPHA
    put(88, 8);   // bits a texel
    put(104, 0xFF);
    d[128 + 128 * 3 + 5] = static_cast<char>(200);
    const auto tex = parse_dds(d);
    REQUIRE(tex);
    CHECK(tex->alpha_only);
    CHECK(tex->format == VK_FORMAT_R8_UNORM);
    CHECK(tex->width == 128);
    CHECK(tex->height == 4);
    REQUIRE(tex->mips.size() == 1);
    CHECK(tex->mips[0].size == 128u * 4u);
    CHECK(static_cast<unsigned char>(tex->mips[0].data[128 * 3 + 5]) == 200);

    // An 8-bit texture of another kind isn't alpha alone
    put(80, 0x40); // DDPF_RGB
    const auto rgb = parse_dds(d);
    REQUIRE(rgb);
    CHECK_FALSE(rgb->alpha_only);
}

namespace {

/// A 2D DDS header in `fourcc` declaring w x h and `mips` levels, followed by
/// `payload` zero bytes.
std::vector<char> declared(const char* fourcc, u32 w, u32 h, u32 mips, size_t payload) {
    std::vector<char> d(128 + payload, 0);
    const auto put = [&](size_t offset, u32 v) { std::memcpy(d.data() + offset, &v, 4); };
    std::memcpy(d.data(), "DDS ", 4);
    put(4, 124);
    put(12, h);
    put(16, w);
    put(28, mips);
    put(76, 32);
    put(80, 0x4); // FourCC
    std::memcpy(d.data() + 84, fourcc, 4);
    return d;
}

} // namespace

TEST_CASE("A DDS declaring more than it holds is refused, however large it claims to be", "[dds]") {
    // 65536 x 65536 DXT5 is 4 GiB of blocks: in 32 bits that wrapped to an
    // empty level, which passed the truncation check.
    CHECK_FALSE(parse_dds(declared("DXT5", 65536, 65536, 1, 16)));
    // A width at the top of u32 wrapped its block count to nothing.
    CHECK_FALSE(parse_dds(declared("DXT1", 0xFFFFFFFFu, 4, 1, 8)));
    // Past the side limit, even with the bytes there.
    CHECK_FALSE(parse_dds(declared("DXT1", 16388, 4, 1, 4097 * 8)));
    // Within it but short of the first level.
    CHECK_FALSE(parse_dds(declared("DXT1", 16384, 16384, 1, 8)));
    // dds_to_rgba refuses rather than sizing an image from the header.
    osc::u32 w = 0;
    osc::u32 h = 0;
    CHECK_FALSE(osc::renderer::dds_to_rgba(declared("DXT5", 65536, 65536, 1, 16), w, h));
}

TEST_CASE("A DDS mip count past a full chain stops at 1x1", "[dds]") {
    // 4x4 DXT1: a full chain is 4, 2, 1, a block each. The count in the
    // header is the file's word; reserving for it asked for ~100 GB.
    const auto tex = parse_dds(declared("DXT1", 4, 4, 0xFFFFFFFFu, 3 * 8 + 64));
    REQUIRE(tex);
    CHECK(tex->mip_count == 3);
    REQUIRE(tex->mips.size() == 3);
    CHECK(tex->mips[2].width == 1);
    CHECK(tex->mips[2].height == 1);
    // A non-square texture's chain runs to its longer side: 8x2 is 8x2, 4x1, 2x1, 1x1.
    const auto wide = parse_dds(declared("DXT1", 8, 2, 99, 4 * 16));
    REQUIRE(wide);
    CHECK(wide->mip_count == 4);
}
