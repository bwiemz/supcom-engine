#include <catch2/catch_test_macros.hpp>

#include "renderer/normal_overlay.hpp"

#include <cmath>
#include <vector>

using namespace osc;
using namespace osc::renderer;

/// Build a 4x4 RGBA texture where every pixel has the same G and A values.
/// DXT5nm: G = normal X, A = normal Y. R and B are unused.
static PredecodedNormal make_uniform_normal_texture(
    const std::string& path, u8 green, u8 alpha, u32 w = 4, u32 h = 4)
{
    PredecodedNormal pd;
    pd.path = path;
    pd.width = w;
    pd.height = h;
    pd.pixels.resize(w * h * 4);
    for (u32 i = 0; i < w * h; ++i) {
        pd.pixels[i * 4 + 0] = 0;     // R (unused)
        pd.pixels[i * 4 + 1] = green; // G = normal X
        pd.pixels[i * 4 + 2] = 0;     // B (unused)
        pd.pixels[i * 4 + 3] = alpha; // A = normal Y
    }
    return pd;
}

TEST_CASE("bake_normal_overlay: empty decal list produces zero overlay", "[normal_overlay]") {
    std::vector<NormalDecalInfo> decals;
    std::vector<PredecodedNormal> predecoded;

    auto overlay = bake_normal_overlay_with_predecoded(decals, 16, 16, predecoded);

    REQUIRE(overlay.width == 16);
    REQUIRE(overlay.height == 16);
    REQUIRE(overlay.pixels.size() == 16 * 16 * 2);

    for (u32 i = 0; i < overlay.pixels.size(); ++i) {
        CHECK(overlay.pixels[i] == 0.0f);
    }
}

TEST_CASE("bake_normal_overlay: single centered decal produces non-zero perturbation",
          "[normal_overlay]")
{
    // Texture with G=200, A=180 → nx = 200/127.5 - 1 ≈ 0.569, ny = 180/127.5 - 1 ≈ 0.412
    const std::string tex_path = "/textures/test_normal.dds";
    auto pd = make_uniform_normal_texture(tex_path, 200, 180);

    NormalDecalInfo decal;
    decal.texture_path = tex_path;
    decal.position_x = 8.0f; // Center of a 16x16 overlay
    decal.position_z = 8.0f;
    decal.scale_x = 4.0f;
    decal.scale_z = 4.0f;
    decal.rotation_y = 0.0f; // No rotation

    auto overlay = bake_normal_overlay_with_predecoded({decal}, 16, 16, {pd});

    REQUIRE(overlay.width == 16);
    REQUIRE(overlay.height == 16);

    // The center texel (8, 8) should have non-zero perturbation.
    const u32 center_idx = (8 * 16 + 8) * 2;
    const f32 expected_nx = 200.0f / 127.5f - 1.0f;
    const f32 expected_ny = 180.0f / 127.5f - 1.0f;

    CHECK(std::abs(overlay.pixels[center_idx + 0] - expected_nx) < 0.01f);
    CHECK(std::abs(overlay.pixels[center_idx + 1] - expected_ny) < 0.01f);

    // A texel far outside the decal footprint (0, 0) should be zero.
    const u32 corner_idx = (0 * 16 + 0) * 2;
    CHECK(overlay.pixels[corner_idx + 0] == 0.0f);
    CHECK(overlay.pixels[corner_idx + 1] == 0.0f);
}

TEST_CASE("bake_normal_overlay: decal outside map bounds causes no crash", "[normal_overlay]") {
    const std::string tex_path = "/textures/offscreen.dds";
    auto pd = make_uniform_normal_texture(tex_path, 255, 255);

    NormalDecalInfo decal;
    decal.texture_path = tex_path;
    decal.position_x = -100.0f; // Far outside the overlay
    decal.position_z = -100.0f;
    decal.scale_x = 4.0f;
    decal.scale_z = 4.0f;
    decal.rotation_y = 0.0f;

    auto overlay = bake_normal_overlay_with_predecoded({decal}, 16, 16, {pd});

    REQUIRE(overlay.width == 16);
    REQUIRE(overlay.height == 16);

    // Entire overlay should remain zero — decal is off-map.
    for (u32 i = 0; i < overlay.pixels.size(); ++i) {
        CHECK(overlay.pixels[i] == 0.0f);
    }
}
