// Build templates (Moho's CWldSession::GenerateBuildTemplates,
// RUnitBlueprint::GetSkirtRect).

#include <catch2/catch_test_macros.hpp>

#include "renderer/build_template.hpp"

#include <vector>

using osc::renderer::generate_build_template;
using osc::renderer::skirt_rect;
using osc::renderer::TemplateStructure;

namespace {

TemplateStructure structure(const char* bp, osc::u32 tick, osc::f32 x, osc::f32 z,
                            osc::f32 foot = 2, osc::f32 skirt = 0, osc::f32 skirt_off = 0) {
    TemplateStructure s;
    s.blueprint_id = bp;
    s.creation_tick = tick;
    s.x = x;
    s.z = z;
    s.foot_x = s.foot_z = foot;
    s.skirt_x = s.skirt_z = skirt;
    s.skirt_off_x = s.skirt_off_z = skirt_off;
    return s;
}

} // namespace

TEST_CASE("A structure's skirt: its footprint's corner on the grid, then the skirt",
          "[build_template]") {
    // A 2x2 at (11, 21): footprint 10..12 by 20..22
    auto r = skirt_rect(structure("a", 0, 11, 21));
    CHECK(r == std::array<osc::f32, 4>{10, 20, 12, 22});
    // A 6x6 skirt offset -2 round it
    r = skirt_rect(structure("a", 0, 11, 21, 2, 6, -2));
    CHECK(r == std::array<osc::f32, 4>{8, 18, 14, 24});
    // A 1x1 at a cell's middle: its corner rounds to the cell
    r = skirt_rect(structure("a", 0, 5.5f, 7.5f, 1));
    CHECK(r == std::array<osc::f32, 4>{5, 7, 6, 8});
}

TEST_CASE("A template: its structures in the order they were made, from the first",
          "[build_template]") {
    std::vector<TemplateStructure> s = {
        structure("ueb1101", 40, 30, 10),            // made later
        structure("ueb0101", 12, 20, 10, 8, 10, -1), // made first: the origin
        structure("ueb1101", 25, 30, 20),
    };
    const auto t = generate_build_template(s);
    REQUIRE(t);
    REQUIRE(t->entries.size() == 3);
    CHECK(t->entries[0].blueprint_id == "ueb0101");
    CHECK(t->entries[0].build_order == 12);
    CHECK(t->entries[0].x == 0);
    CHECK(t->entries[0].z == 0);
    CHECK(t->entries[1].build_order == 25);
    CHECK(t->entries[1].x == 10);
    CHECK(t->entries[1].z == 10);
    CHECK(t->entries[2].build_order == 40);
    CHECK(t->entries[2].x == 10);
    CHECK(t->entries[2].z == 0);
    // Spans: the factory's skirt 15..25 by 5..15 and the two 2x2s
    // (29..31 by 9..11 and 19..21): x 15..31, z 5..21
    CHECK(t->span_x == 16);
    CHECK(t->span_z == 16);
}

TEST_CASE("No structures make no template", "[build_template]") {
    CHECK_FALSE(generate_build_template({}).has_value());
}
