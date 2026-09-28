#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "map/scmap_parser.hpp"
#include "renderer/sky_dome.hpp"

#include <cmath>
#include <set>

using namespace osc;
using Catch::Matchers::WithinAbs;

namespace {

/// SCMP_009's dome (its .scmap's sky block).
map::ScmapSky scmp_009() {
    map::ScmapSky sky;
    sky.origin[0] = 512;
    sky.origin[2] = 512;
    sky.elevation = -42.5f;
    sky.radius = 2343.1628f;
    sky.start_angle = 1.2566370f;
    return sky;
}

} // namespace

TEST_CASE("The sky dome is Moho's rings and apex (M210b)", "[sky]") {
    const map::ScmapSky sky = scmp_009();
    const renderer::SkyDomeMesh dome = renderer::build_sky_dome(sky);
    // height rings of width + 1, then the apex; width * (6 (height - 1) + 3) indices
    REQUIRE(dome.vertices.size() == 6u * 17u + 1u);
    REQUIRE(dome.indices.size() == 16u * (6u * 5u + 3u));

    const f32 r = sky.radius / std::cos(sky.start_angle);
    const f32 base = r * std::sin(sky.start_angle);
    const f32 step = (1.5707964f - sky.start_angle) / 6.0f;
    for (int ring = 0; ring < 6; ++ring) {
        const f32 a = sky.start_angle + static_cast<f32>(ring) * step;
        for (int c = 0; c <= 16; ++c) {
            const renderer::SkyDomeVertex& v =
                dome.vertices[static_cast<size_t>(ring) * 17 + static_cast<size_t>(c)];
            const f32 theta = static_cast<f32>(c) / 16.0f * 6.2831855f;
            CHECK_THAT(v.theta, WithinAbs(theta, 1e-5));
            CHECK_THAT(v.pos[0], WithinAbs(512.0 + std::cos(theta) * std::cos(a) * r, 0.01));
            CHECK_THAT(v.pos[2], WithinAbs(512.0 + std::sin(theta) * std::cos(a) * r, 0.01));
            CHECK_THAT(v.pos[1], WithinAbs(std::sin(a) * r - base - 42.5, 0.01));
        }
    }
    // The lowest ring is the radius out, at the elevation
    CHECK_THAT(dome.vertices[0].pos[0], WithinAbs(512.0 + 2343.1628, 0.05));
    CHECK_THAT(dome.vertices[0].pos[1], WithinAbs(-42.5, 0.01));
    // The apex, over the origin
    const renderer::SkyDomeVertex& apex = dome.vertices.back();
    CHECK_THAT(apex.pos[0], WithinAbs(512.0, 1e-3));
    CHECK_THAT(apex.pos[2], WithinAbs(512.0, 1e-3));
    CHECK_THAT(apex.pos[1], WithinAbs(r - base - 42.5, 0.01));
    CHECK(apex.pos[1] > dome.vertices[5 * 17].pos[1]);
}

TEST_CASE("The sky dome's indices: its ring pairs' quads, then the fan to the apex (M210b)",
          "[sky]") {
    const renderer::SkyDomeMesh dome = renderer::build_sky_dome(scmp_009());
    // The first quad: (1, 17, 0) and (17, 1, 18)
    const std::vector<u16> first(dome.indices.begin(), dome.indices.begin() + 6);
    CHECK(first == std::vector<u16>{1, 17, 0, 17, 1, 18});
    // The next ring pair starts at 17
    const size_t second_ring = 16 * 6;
    CHECK(dome.indices[second_ring] == 18);
    CHECK(dome.indices[second_ring + 2] == 17);
    // The fan: (cap + c + 1, apex, cap + c), the cap the top ring (85)
    const size_t fan = 16u * 6u * 5u;
    const u16 apex = static_cast<u16>(dome.vertices.size() - 1);
    for (u16 c = 0; c < 16; ++c) {
        CHECK(dome.indices[fan + c * 3u] == 85 + c + 1);
        CHECK(dome.indices[fan + c * 3u + 1] == apex);
        CHECK(dome.indices[fan + c * 3u + 2] == 85 + c);
    }
    // Every vertex is used, none out of range
    const std::set<u16> used(dome.indices.begin(), dome.indices.end());
    CHECK(*used.rbegin() == apex);
    CHECK(used.size() == dome.vertices.size());
}

TEST_CASE("A sky dome that can't be drawn is empty (M210b)", "[sky]") {
    map::ScmapSky sky = scmp_009();
    sky.width = 0;
    CHECK(renderer::build_sky_dome(sky).indices.empty());
    sky = scmp_009();
    sky.height = 0;
    CHECK(renderer::build_sky_dome(sky).vertices.empty());
    // More vertices than 16-bit indices reach
    sky = scmp_009();
    sky.width = 1000;
    sky.height = 100;
    CHECK(renderer::build_sky_dome(sky).indices.empty());
    // One ring is a fan alone
    sky = scmp_009();
    sky.height = 1;
    const renderer::SkyDomeMesh fan = renderer::build_sky_dome(sky);
    CHECK(fan.vertices.size() == 18u);
    CHECK(fan.indices.size() == 16u * 3u);
}

TEST_CASE("An older map's sky is SetupHorizonAndCirrus's (M210b)", "[sky]") {
    const map::ScmapSky sky = map::default_sky(1024, 512, 17.5f);
    CHECK_THAT(sky.origin[0], WithinAbs(512.0, 1e-4));
    CHECK_THAT(sky.origin[1], WithinAbs(0.0, 1e-6));
    CHECK_THAT(sky.origin[2], WithinAbs(256.0, 1e-4));
    const double radius = std::sqrt(512.0 * 512.0 + 256.0 * 256.0) / std::cos(1.25663697719574);
    CHECK_THAT(sky.radius, WithinAbs(radius, 0.01));
    CHECK_THAT(sky.elevation, WithinAbs(17.5, 1e-6));
    CHECK_THAT(sky.start_angle, WithinAbs(1.2566371, 1e-6));
    CHECK(sky.width == 16);
    CHECK(sky.height == 6);
    CHECK_THAT(sky.horizon_size, WithinAbs(radius * 0.1536, 0.01));
    CHECK_THAT(sky.horizon_color[0], WithinAbs(0.81, 1e-6));
    CHECK_THAT(sky.sky_color[2], WithinAbs(0.59, 1e-6));
    CHECK_THAT(sky.cirrus_multiplier, WithinAbs(1.8, 1e-6));
    CHECK_THAT(sky.cirrus_color[1], WithinAbs(0.76, 1e-6));
    CHECK(sky.cirrus_texture == "/textures/environment/cirrus001_512.dds");
    CHECK(sky.decals.empty());
    CHECK(sky.decal_albedo.empty());
    // FA's static cirrus table
    CHECK_THAT(sky.cirrus[0].frequency[0], WithinAbs(0.00428, 1e-6));
    CHECK_THAT(sky.cirrus[3].direction[1], WithinAbs(-0.80386, 1e-6));
}
