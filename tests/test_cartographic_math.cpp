// The cartographic view's lookup data, as Moho's Cartographic builds it
// (faf-re render/Cartographic.cpp): constants, the elevation texture, and
// the hypsometric and topographic ramps.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "map/heightmap.hpp"
#include "map/terrain.hpp"
#include "renderer/cartographic_math.hpp"

#include <algorithm>
#include <cstdint>
#include <set>
#include <utility>
#include <vector>

using osc::f32;
using osc::i32;
using osc::u16;
using osc::u32;
using osc::u8;
using namespace osc::renderer;

namespace {

/// A size x size map whose height word at sample (x, z) is word(x, z),
/// at Moho's 1/128 a word.
template <typename Word>
osc::map::Terrain terrain(u32 size, Word word, bool water = false, f32 surface = 0, f32 deep = 0) {
    std::vector<u16> heights;
    for (u32 z = 0; z <= size; ++z)
        for (u32 x = 0; x <= size; ++x) heights.push_back(word(x, z));
    osc::map::Terrain t(osc::map::Heightmap(size, size, 1.0f / 128.0f, std::move(heights)), surface,
                        water);
    t.set_water_deep_elevation(deep);
    return t;
}

} // namespace

TEST_CASE("The cartographic grid is half the heightfield's span", "[cartographic]") {
    const auto t = terrain(512, [](u32 x, u32) { return static_cast<u16>(1000 + x); });
    const CartographicParams p = cartographic_params(t);
    CHECK(p.grid_width == 256);
    CHECK(p.grid_height == 256);
    CHECK(p.grid_size_coeff[0] == 1.0f / 256.0f);
    CHECK(p.grid_size_coeff[3] == 1.0f);
    CHECK(p.terrain_size_coeff[0] == 0.5f);
    CHECK(p.terrain_size_coeff[1] == 0.5f);
    // the heights' floor and ceiling, in world units
    CHECK(p.elev_min == 1000.0f / 128.0f);
    CHECK(p.elev_max == 1512.0f / 128.0f);
    // the map's own colours and bands (Moho's defaults here)
    CHECK(p.palette[0] == 0xFF0E3EFFu);
    CHECK(p.bands == 20);
}

TEST_CASE("The water lanes: the surface, and deep water's start twice", "[cartographic]") {
    const auto word = [](u32 x, u32) { return static_cast<u16>(x * 128); };
    const auto wet = terrain(16, word, true, 10.0f, 6.0f);
    const CartographicParams p = cartographic_params(wet);
    CHECK(p.has_bounds);
    CHECK(p.surface == 10.0f);
    CHECK(p.water == 6.0f);
    CHECK(p.deep_lane == 6.0f); // Moho reads deep water's start for the abyss lane too
    const auto dry = terrain(16, word);
    const CartographicParams q = cartographic_params(dry);
    CHECK_FALSE(q.has_bounds);
    CHECK(q.surface == 0.0f);
    CHECK(q.water == 0.0f);
    CHECK(q.deep_lane == 0.0f);
}

TEST_CASE("The band count is held to 1 to 100", "[cartographic]") {
    auto t = terrain(16, [](u32, u32) { return u16{0}; });
    osc::map::ScmapCartographic c;
    c.topographic_samples = 0;
    t.set_cartographic(c);
    CHECK(cartographic_params(t).bands == 1);
    c.topographic_samples = 500;
    t.set_cartographic(c);
    CHECK(cartographic_params(t).bands == 100);
}

TEST_CASE("The elevation kernel reads the sample up and left, clamped", "[cartographic]") {
    // height x at sample x (word x * 128)
    const auto t = terrain(16, [](u32 x, u32 z) { return static_cast<u16>((x + 20 * z) * 128); });
    const auto height = [](i32 x, i32 z) { return static_cast<f32>(x + 20 * z); };
    CHECK(cartographic_height(t, 5, 3, 0.0f, 1.0f) == Catch::Approx(height(4, 2)));
    CHECK(cartographic_height(t, 0, 0, 0.0f, 1.0f) == Catch::Approx(height(0, 0)));
    CHECK(cartographic_height(t, 40, 40, 0.0f, 1.0f) == Catch::Approx(height(16, 16)));
    CHECK(cartographic_height(t, 5, 3, 10.0f, 0.5f) ==
          Catch::Approx((height(4, 2) - 10.0f) * 0.5f));
}

TEST_CASE("The elevation texture: half floats, a texel's sample stepped across the field",
          "[cartographic]") {
    // height x at sample x: 0 to 16, so a sample's value is x / 16
    const auto t = terrain(16, [](u32 x, u32) { return static_cast<u16>(x * 128); });
    const CartographicParams p = cartographic_params(t);
    const std::vector<u16> texels = cartographic_elevation(t, p);
    REQUIRE(p.grid_width == 8);
    REQUIRE(texels.size() == 64);
    // texel x steps 16/7 samples and reads the sample before: 0, 1, 3, 5 ...
    CHECK(texels[0] == float_to_half(0.0f));
    CHECK(texels[1] == float_to_half(1.0f / 16.0f));
    CHECK(texels[2] == float_to_half(3.0f / 16.0f));
    CHECK(texels[7] == float_to_half(15.0f / 16.0f)); // never the last sample
    // every row alike
    CHECK(std::equal(texels.begin(), texels.begin() + 8, texels.begin() + 56));
}

TEST_CASE("Half floats round half up, keep denormals and saturate", "[cartographic]") {
    CHECK(float_to_half(0.0f) == 0x0000);
    CHECK(float_to_half(-0.0f) == 0x8000);
    CHECK(float_to_half(1.0f) == 0x3C00);
    CHECK(float_to_half(0.5f) == 0x3800);
    CHECK(float_to_half(-2.0f) == 0xC000);
    CHECK(float_to_half(1.0f + 1.0f / 2048.0f) == 0x3C01); // the tie rounds up
    CHECK(float_to_half(1.0f + 1.0f / 4096.0f) == 0x3C00);
    CHECK(float_to_half(1.0f / 32768.0f) == 0x0200); // a denormal
    CHECK(float_to_half(1e-9f) == 0x0000);
    CHECK(float_to_half(70000.0f) == 0x7C00);
}

TEST_CASE("BlendPackedColor truncates every byte, alpha too", "[cartographic]") {
    CHECK(blend_packed_color(0xFF000000u, 0xFFFFFFFFu, 0.5f) == 0xFF7F7F7Fu);
    CHECK(blend_packed_color(0x00000000u, 0xFF000000u, 0.5f) == 0x7F000000u);
    CHECK(blend_packed_color(0xFF4C9D32u, 0xFFFFFFFFu, 0.0f) == 0xFF4C9D32u);
    CHECK(blend_packed_color(0xFF4C9D32u, 0xFFFFFFFFu, 1.0f) == 0xFFFFFFFFu);
    CHECK(truncate_to_i32(-1.5f) == -1);
    CHECK(truncate_to_i32(1e20f) == INT32_MIN);
}

namespace {

CartographicParams ramp_params(bool water) {
    CartographicParams p;
    p.elev_min = 0;
    p.elev_max = 100;
    p.has_bounds = water;
    p.surface = water ? 30.0f : 0.0f;
    p.water = water ? 20.0f : 0.0f;
    p.deep_lane = p.water;
    p.palette = {0xFF000001u, 0xFF000002u, 0xFF000003u, 0xFF000400u, 0xFF00FF00u};
    p.bands = 20;
    return p;
}

} // namespace

TEST_CASE("The colour ramp: the water's blues, then low land to high", "[cartographic]") {
    const CartographicParams p = ramp_params(true);
    const auto ramp = hypsometric_ramp(p);
    CHECK(ramp[0] == p.palette[0]);
    CHECK(ramp[40] == p.palette[0]); // 0.157: under deep water's start
    CHECK(ramp[60] == p.palette[2]); // 0.235: between it and the surface
    // with the abyss lane read from deep water, the second blue never shows
    CHECK(std::find(ramp.begin(), ramp.end(), p.palette[1]) == ramp.end());
    CHECK(ramp[100] ==
          blend_packed_color(p.palette[3], p.palette[4], (100.0f / 255.0f - 0.3f) / 0.7f));
    CHECK(ramp[255] == p.palette[4]);

    // an abyss lane of its own shows it
    CartographicParams abyss = p;
    abyss.deep_lane = 10.0f;
    const auto three = hypsometric_ramp(abyss);
    CHECK(three[20] == p.palette[0]); // 0.078
    CHECK(three[40] == p.palette[1]); // 0.157
    CHECK(three[60] == p.palette[2]);

    // a dry map is land throughout
    const auto dry = hypsometric_ramp(ramp_params(false));
    CHECK(dry[0] == p.palette[3]);
    CHECK(dry[255] == p.palette[4]);
}

TEST_CASE("The contour ramp: none under water, a shade a band on land", "[cartographic]") {
    const CartographicParams p = ramp_params(true);
    const auto ramp = topographic_ramp(p);
    for (int i = 0; i <= 50; ++i) CHECK(ramp[i] == 0); // 0.196: under deep water's start
    std::set<u8> shades;
    for (int i = 53; i < 255; ++i) {
        CHECK(ramp[i] >= 16);
        CHECK(ramp[i] >= ramp[i - 1]);
        shades.insert(ramp[i]);
    }
    CHECK(shades.size() == 20);
    // the first band: 16 + 239 / 20, truncated
    CHECK(ramp[53] == 27);

    // one band: all land the brightest shade
    CartographicParams one = p;
    one.bands = 1;
    const auto flat = topographic_ramp(one);
    for (int i = 60; i < 250; ++i) CHECK(flat[i] == 255);

    // a dry map has contours from its floor
    const auto dry = topographic_ramp(ramp_params(false));
    CHECK(dry[0] == 27);
}

TEST_CASE("The cartographic view: the rect fills the target, higher is nearer", "[cartographic]") {
    const auto m = cartographic_view_proj(100.0f, 50.0f, 300.0f, 150.0f, 0.0f, 40.0f);
    // column-major: clip = m * (x, y, z, 1)
    const auto clip = [&](f32 x, f32 y, f32 z) {
        std::array<f32, 3> out{};
        for (size_t row = 0; row < 3; ++row)
            out[row] = m[row] * x + m[4 + row] * y + m[8 + row] * z + m[12 + row];
        return out;
    };
    const auto top_left = clip(100.0f, 0.0f, 50.0f);
    CHECK(top_left[0] == Catch::Approx(-1.0f));
    CHECK(top_left[1] == Catch::Approx(-1.0f)); // z0 at the top
    const auto bottom_right = clip(300.0f, 0.0f, 150.0f);
    CHECK(bottom_right[0] == Catch::Approx(1.0f));
    CHECK(bottom_right[1] == Catch::Approx(1.0f));
    CHECK(clip(200.0f, 40.0f, 100.0f)[2] == Catch::Approx(0.0f));
    CHECK(clip(200.0f, 0.0f, 100.0f)[2] == Catch::Approx(1.0f));
    CHECK(m[15] == 1.0f);
}

TEST_CASE("The ground quad lies at the surface, in whole height words", "[cartographic]") {
    CartographicParams p;
    p.surface = 12.34f; // 1579.52 words
    CHECK(cartographic_surface_word(p) == 1579);
    p.surface = -0.5f;
    CHECK(cartographic_surface_word(p) == -64);
}
