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

namespace {

using osc::renderer::BuildTemplate;
using osc::renderer::template_sites;
using osc::renderer::TemplateSite;

/// Footprints by blueprint: "one" 1x1, "two" 2x2, "three" 3x3.
std::array<osc::f32, 2> foot(const std::string& bp) {
    if (bp == "two") return {2, 2};
    if (bp == "three") return {3, 3};
    return {1, 1};
}

BuildTemplate make(osc::f32 span_x, osc::f32 span_z,
                   std::vector<osc::renderer::BuildTemplateEntry> entries) {
    BuildTemplate t;
    t.span_x = span_x;
    t.span_z = span_z;
    t.entries = std::move(entries);
    return t;
}

bool at(const TemplateSite& s, const char* bp, osc::f32 x, osc::f32 z) {
    return s.blueprint_id == bp && s.x == x && s.z == z;
}

} // namespace

TEST_CASE("A template's structures go from the centre of the 1x1 cell under its lead's corner, "
          "each snapped by its own footprint",
          "[build_template]") {
    // Lead 1x1: its corner cell lrint(10.3 - 0.5) = 10, lrint(20.7 - 0.5) =
    // 20, so the anchor is (10.5, 20.5); a 2x2 four and two from it snaps
    // to nearbyint(13.5) + 1 = 15 (ties to even), nearbyint(21.5) + 1 = 23.
    const auto t = make(8, 6, {{"one", 1, 0, 0}, {"two", 2, 4, 2}});
    const auto sites = template_sites(t, 10.3f, 20.7f, 10.3f, 20.7f, false, foot);
    REQUIRE(sites.size() == 2);
    CHECK(at(sites[0], "one", 10.5f, 20.5f));
    CHECK(at(sites[1], "two", 15.0f, 23.0f));
}

TEST_CASE("A template whose lead is 2x2 anchors on a 1x1 cell, not where the lead alone would "
          "snap",
          "[build_template]") {
    // Corner cell lrint(9.3) = 9, lrint(19.7) = 20: anchor (9.5, 20.5), so
    // the lead snaps to nearbyint(8.5) + 1 = 9, nearbyint(19.5) + 1 = 21 --
    // a lone 2x2 at the same cursor would stand at (10, 21). Moho's
    // CBuildDragPreview places the stamp by the 1x1 cell.
    const auto t = make(4, 4, {{"two", 1, 0, 0}, {"three", 2, -6, 0}});
    const auto sites = template_sites(t, 10.3f, 20.7f, 10.3f, 20.7f, false, foot);
    REQUIRE(sites.size() == 2);
    CHECK(at(sites[0], "two", 9.0f, 21.0f));
    // The 3x3: (3.5, 20.5) snaps to nearbyint(2.0) + 1.5, nearbyint(19.0) + 1.5
    CHECK(at(sites[1], "three", 3.5f, 20.5f));
}

TEST_CASE("A template dragged lays copies a span apart along the drag's longer axis, the other "
          "following, floor(length / span) + 1 of them",
          "[build_template]") {
    const auto t = make(4, 2, {{"one", 1, 0, 0}});
    // Cells (10, 20) to (19, 21): 9 along x, so spanX (4) apart, 3 copies,
    // z rising 1 over the drag: 20 + 1 * 4/9 and 8/9, rounded.
    const auto sites = template_sites(t, 10.5f, 20.5f, 19.5f, 21.5f, true, foot);
    REQUIRE(sites.size() == 3);
    CHECK(at(sites[0], "one", 10.5f, 20.5f));
    CHECK(at(sites[1], "one", 14.5f, 20.5f));
    CHECK(at(sites[2], "one", 18.5f, 21.5f));
    // More in z: spanZ (2) apart, 5 / 2 + 1 = 3 copies
    const auto down = template_sites(t, 10.5f, 20.5f, 10.5f, 25.5f, true, foot);
    REQUIRE(down.size() == 3);
    CHECK(at(down[2], "one", 10.5f, 24.5f));
    // A lead that isn't DRAGBUILD: the press alone
    CHECK(template_sites(t, 10.5f, 20.5f, 19.5f, 21.5f, false, foot).size() == 1);
}

TEST_CASE("A template's copies go copy by copy, each in the template's order", "[build_template]") {
    const auto t = make(3, 3, {{"one", 1, 0, 0}, {"two", 2, 1, 0}});
    const auto sites = template_sites(t, 0.5f, 0.5f, 6.5f, 0.5f, true, foot);
    REQUIRE(sites.size() == 6);
    for (size_t i = 0; i < sites.size(); ++i)
        CHECK(sites[i].blueprint_id == (i % 2 == 0 ? "one" : "two"));
    CHECK(template_sites(BuildTemplate{}, 0, 0, 0, 0, false, foot).empty());
    // A span under a cell: one copy, however long the drag
    CHECK(template_sites(make(0.001f, 0.001f, {{"one", 1, 0, 0}}), 0.5f, 0.5f, 900.5f, 0.5f, true,
                         foot)
              .size() == 1);
}
