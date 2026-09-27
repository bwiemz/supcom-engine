#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "renderer/camera.hpp"

#include <array>
#include <cmath>

using namespace osc;
using namespace osc::renderer;
using Catch::Matchers::WithinAbs;

namespace {

/// A world point through a column-major view-projection (orthographic, so
/// w stays 1): x and y in [-1, 1] across the shadow map, z its depth.
std::array<f32, 3> project(const std::array<f32, 16>& m, f32 x, f32 y, f32 z) {
    return {m[0] * x + m[4] * y + m[8] * z + m[12], m[1] * x + m[5] * y + m[9] * z + m[13],
            m[2] * x + m[6] * y + m[10] * z + m[14]};
}

} // namespace

TEST_CASE("The shadow map looks down the map's sun (M210a)", "[renderer][shadow]") {
    const f32 sun[3] = {0.616f, 0.559f, 0.555f}; // SCMP_009's
    const auto vp = math::light_view_proj(sun, 300.0f, 200.0f, 50.0f);

    // The target is the map's centre.
    const auto c = project(vp, 300.0f, 0.0f, 200.0f);
    CHECK_THAT(c[0], WithinAbs(0.0, 1e-4));
    CHECK_THAT(c[1], WithinAbs(0.0, 1e-4));
    // A point toward the sun is nearer the light, at the same place on the map.
    const auto up_sun = project(vp, 300.0f + sun[0] * 10, sun[1] * 10, 200.0f + sun[2] * 10);
    CHECK(up_sun[2] < c[2]);
    CHECK_THAT(up_sun[0], WithinAbs(c[0], 1e-4));
    CHECK_THAT(up_sun[1], WithinAbs(c[1], 1e-4));
}

TEST_CASE("An overhead sun's shadow map still spreads the ground over it (M210a)",
          "[renderer][shadow]") {
    // Straight down, +Y is the view direction: taken as up, it would put
    // every point of the ground on one texel.
    const f32 sun[3] = {0.0f, 1.0f, 0.0f};
    const auto vp = math::light_view_proj(sun, 300.0f, 200.0f, 50.0f);
    const auto east = project(vp, 310.0f, 0.0f, 200.0f);
    const auto north = project(vp, 300.0f, 0.0f, 210.0f);
    const auto c = project(vp, 300.0f, 0.0f, 200.0f);
    // 10 of the box's 50 each way: a fifth of the map from its centre,
    // along one axis each.
    const f32 de = std::abs(east[0] - c[0]) + std::abs(east[1] - c[1]);
    const f32 dn = std::abs(north[0] - c[0]) + std::abs(north[1] - c[1]);
    CHECK_THAT(de, WithinAbs(0.2, 1e-4));
    CHECK_THAT(dn, WithinAbs(0.2, 1e-4));
    CHECK((std::abs(east[0] - north[0]) > 0.1f || std::abs(east[1] - north[1]) > 0.1f));
}

TEST_CASE("The shadow map's view takes the sun's direction at any length (M210a)",
          "[renderer][shadow]") {
    const f32 unit[3] = {0.0f, 0.8f, 0.6f};
    const f32 longer[3] = {0.0f, 1.6f, 1.2f};
    const auto a = math::light_view_proj(unit, 300.0f, 200.0f, 50.0f);
    const auto b = math::light_view_proj(longer, 300.0f, 200.0f, 50.0f);
    for (size_t i = 0; i < a.size(); ++i) CHECK_THAT(a[i], WithinAbs(b[i], 1e-5));
}
