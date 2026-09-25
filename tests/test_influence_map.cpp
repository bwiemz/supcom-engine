// An army's influence map (M207b): Moho's CInfluenceMap, by its rules from
// the decompile (docs/plans/2026-09-25-m207b-influence-map-design.md).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "sim/influence_map.hpp"

#include <optional>

using namespace osc;
using Catch::Matchers::WithinAbs;
using osc::sim::CellRect;
using osc::sim::InfluenceMap;
using osc::sim::ThreatLayer;
using osc::sim::ThreatSource;
using osc::sim::ThreatType;

namespace {

/// A tank: 10 surface threat, driving on land.
ThreatSource tank() {
    ThreatSource s;
    s.surface = 10;
    s.mobile = true;
    return s;
}

/// A structure: 4 surface and 1 economy threat.
ThreatSource structure() {
    ThreatSource s;
    s.surface = 4;
    s.economy = 1;
    return s;
}

/// The map's army 0 (the owner, allied with army 2) updating, with every
/// enemy unit on land and detailed as given.
void update(InfluenceMap& map, bool detailed = true, ThreatLayer layer = ThreatLayer::Land) {
    map.update([](i32 army) { return army == 0 || army == 2; },
               [&](u32) { return std::optional<InfluenceMap::UnitState>({layer, detailed}); });
}

f32 cell(const InfluenceMap& map, f32 x, f32 z, ThreatType type, i32 army = -1) {
    const i32 c = map.cell_of({x, 0, z});
    return map.cell_threat(c % map.width(), c / map.width(), type, army);
}

} // namespace

TEST_CASE("An influence map's cells are sized to the map, as Moho's", "[influence]") {
    const InfluenceMap small(256, 256, 2); // 5 km
    CHECK(small.grid_size() == 32);
    CHECK((small.width() == 8 && small.height() == 8));
    const InfluenceMap mid(512, 512, 2); // 10 km: 16 across from here up
    CHECK(mid.grid_size() == 32);
    CHECK(mid.width() == 16);
    const InfluenceMap big(1024, 512, 2);
    CHECK(big.grid_size() == 64);
    CHECK((big.width() == 16 && big.height() == 8));

    // A position's cell, clamped to the grid; a cell's centre.
    CHECK(mid.cell_of({40, 0, 70}) == 1 + 2 * 16);
    CHECK(mid.cell_of({-5, 0, 9000}) == 15 * 16);
    CHECK(mid.cell_centre(1, 2).x == 48.0f);
    CHECK(mid.cell_centre(1, 2).z == 80.0f);
}

TEST_CASE("A unit's threat holds for 10 updates, then fades; a structure's stays", "[influence]") {
    InfluenceMap map(512, 512, 3);
    map.report(1, 1, {40, 0, 40}, tank());
    map.report(2, 1, {100, 0, 100}, structure());
    update(map);
    CHECK_THAT(cell(map, 40, 40, ThreatType::Overall), WithinAbs(10.0, 1e-5));
    CHECK_THAT(cell(map, 100, 100, ThreatType::Overall), WithinAbs(5.0, 1e-5));

    // Unseen from here: 8 more updates at full strength, then 0.02 less each.
    for (int i = 0; i < 8; ++i) update(map);
    CHECK(map.strength(1) == 1.0f);
    update(map); // the tenth
    CHECK_THAT(map.strength(1), WithinAbs(0.98, 1e-6));
    update(map);
    CHECK_THAT(map.strength(1), WithinAbs(0.96, 1e-6));
    CHECK_THAT(cell(map, 40, 40, ThreatType::Overall), WithinAbs(9.6, 1e-4));

    // Seen again, it is at full strength for another 10.
    map.report(1, 1, {41, 0, 41}, tank());
    update(map);
    CHECK(map.strength(1) == 1.0f);

    // Left unseen, it goes after 49 more fading updates; the structure stays.
    for (int i = 0; i < 70; ++i) update(map);
    CHECK_FALSE(map.has_entry(1));
    CHECK(cell(map, 40, 40, ThreatType::Overall) == 0.0f);
    CHECK(map.strength(2) == 1.0f);
    CHECK_THAT(cell(map, 100, 100, ThreatType::Overall), WithinAbs(5.0, 1e-5));
}

TEST_CASE("A unit that crosses into another cell moves its entry", "[influence]") {
    InfluenceMap map(512, 512, 2);
    map.report(1, 1, {40, 0, 40}, tank());
    update(map);
    map.report(1, 1, {200, 0, 40}, tank());
    update(map);
    CHECK(cell(map, 40, 40, ThreatType::Overall) == 0.0f);
    CHECK_THAT(cell(map, 200, 40, ThreatType::Overall), WithinAbs(10.0, 1e-5));
    CHECK(map.entry_count() == 1);
    map.remove(1);
    CHECK(map.entry_count() == 0);
}

TEST_CASE("Threat is laned by what the unit is and how well it was seen", "[influence]") {
    InfluenceMap map(512, 512, 3);
    ThreatSource mex = structure();
    mex.mass_extractor = true;
    ThreatSource plane;
    plane.air = 3;
    plane.mobile = true;
    plane.flies = true;
    ThreatSource acu = tank();
    acu.commander = true;
    map.report(1, 1, {40, 0, 40}, tank());      // cell A
    map.report(2, 1, {40, 0, 40}, structure()); // cell A
    map.report(3, 1, {40, 0, 40}, mex);         // cell A
    map.report(4, 1, {40, 0, 40}, plane);       // cell A
    map.report(5, 1, {200, 0, 200}, acu);       // cell B
    update(map, true);
    CHECK_THAT(cell(map, 40, 40, ThreatType::Overall), WithinAbs(10 + 5 + 5 + 3, 1e-4));
    CHECK_THAT(cell(map, 40, 40, ThreatType::Structures), WithinAbs(10.0, 1e-5));
    CHECK_THAT(cell(map, 40, 40, ThreatType::StructuresNotMex), WithinAbs(5.0, 1e-5));
    CHECK_THAT(cell(map, 40, 40, ThreatType::Land), WithinAbs(10.0, 1e-5));
    CHECK_THAT(cell(map, 40, 40, ThreatType::Air), WithinAbs(3.0, 1e-5));
    CHECK_THAT(cell(map, 40, 40, ThreatType::AntiSurface), WithinAbs(18.0, 1e-5));
    CHECK_THAT(cell(map, 40, 40, ThreatType::AntiAir), WithinAbs(3.0, 1e-5));
    CHECK_THAT(cell(map, 40, 40, ThreatType::Economy), WithinAbs(2.0, 1e-5));
    CHECK(cell(map, 40, 40, ThreatType::Unknown) == 0.0f);
    CHECK_THAT(cell(map, 200, 200, ThreatType::Commander), WithinAbs(10.0, 1e-5));
    CHECK(cell(map, 200, 200, ThreatType::Artillery) == 0.0f); // Moho's lookup misses

    // At sea it counts as naval.
    InfluenceMap sea(512, 512, 2);
    sea.report(1, 1, {40, 0, 40}, tank());
    update(sea, true, ThreatLayer::Naval);
    CHECK_THAT(cell(sea, 40, 40, ThreatType::Naval), WithinAbs(10.0, 1e-5));
    CHECK(cell(sea, 40, 40, ThreatType::Land) == 0.0f);

    // Never seen closely, a unit's threat is Unknown, not AntiSurface.
    InfluenceMap blip(512, 512, 2);
    blip.report(1, 1, {40, 0, 40}, tank());
    update(blip, false);
    CHECK_THAT(cell(blip, 40, 40, ThreatType::Unknown), WithinAbs(10.0, 1e-5));
    CHECK(cell(blip, 40, 40, ThreatType::AntiSurface) == 0.0f);
    // Once seen closely, it stays detailed.
    update(blip, true);
    update(blip, false);
    CHECK_THAT(cell(blip, 40, 40, ThreatType::AntiSurface), WithinAbs(10.0, 1e-5));
}

TEST_CASE("An ally's units count only as Overall, Unknown, Structures and Air", "[influence]") {
    InfluenceMap map(512, 512, 3);
    map.report(1, 2, {40, 0, 40}, tank()); // army 2 is allied with the owner
    update(map, true);
    CHECK_THAT(cell(map, 40, 40, ThreatType::Overall), WithinAbs(10.0, 1e-5));
    CHECK_THAT(cell(map, 40, 40, ThreatType::Unknown), WithinAbs(10.0, 1e-5));
    CHECK(cell(map, 40, 40, ThreatType::Land) == 0.0f);
    CHECK(cell(map, 40, 40, ThreatType::AntiSurface) == 0.0f);

    // Asked for one army, only its units count.
    map.report(2, 1, {40, 0, 40}, tank());
    update(map, true);
    CHECK_THAT(cell(map, 40, 40, ThreatType::Overall, 1), WithinAbs(10.0, 1e-5));
    CHECK_THAT(cell(map, 40, 40, ThreatType::Overall, 2), WithinAbs(10.0, 1e-5));
    CHECK_THAT(cell(map, 40, 40, ThreatType::Overall), WithinAbs(20.0, 1e-5));
}

TEST_CASE("A script's threat fades by its rate, and shows as Overall after an update",
          "[influence]") {
    InfluenceMap map(512, 512, 2);
    map.assign_threat({40, 0, 40}, ThreatType::Overall, 50, 0.1f);
    CHECK_THAT(cell(map, 40, 40, ThreatType::Unknown), WithinAbs(50.0, 1e-5));
    CHECK(cell(map, 40, 40, ThreatType::Overall) == 0.0f); // not summed until an update
    CHECK(cell(map, 40, 40, ThreatType::OverallNotAssigned) == 0.0f);
    update(map);
    CHECK_THAT(cell(map, 40, 40, ThreatType::Overall), WithinAbs(45.0, 1e-4));
    for (int i = 0; i < 9; ++i) update(map);
    CHECK(cell(map, 40, 40, ThreatType::Overall) == 0.0f); // gone after 1/rate
    // A negative rate is Moho's default, 0.01.
    map.assign_threat({40, 0, 40}, ThreatType::Land, 100, -1);
    update(map);
    CHECK_THAT(cell(map, 40, 40, ThreatType::Land), WithinAbs(99.0, 1e-4));
}

TEST_CASE("Threat queries answer in cells, as Moho's", "[influence]") {
    InfluenceMap map(512, 512, 2);               // 32-unit cells
    map.report(1, 1, {40, 0, 40}, tank());       // cell (1, 1)
    map.report(2, 1, {104, 0, 40}, tank());      // cell (3, 1)
    map.report(3, 1, {40, 0, 200}, structure()); // cell (1, 6)
    update(map);

    // A square of rings about the cell.
    CHECK_THAT(map.threat_rect(1, 1, 0, nullptr, ThreatType::Overall, -1), WithinAbs(10, 1e-5));
    CHECK_THAT(map.threat_rect(2, 1, 1, nullptr, ThreatType::Overall, -1), WithinAbs(20, 1e-5));
    // Clipped to the playable area when on the map.
    const CellRect playable{2, 0, 15, 15};
    CHECK_THAT(map.threat_rect(2, 1, 1, &playable, ThreatType::Overall, -1), WithinAbs(10, 1e-5));

    // The cells with threat about a position, at their centres, highest first.
    const auto rows = map.threats_around({40, 0, 40}, 5, nullptr, ThreatType::Overall, -1);
    REQUIRE(rows.size() == 3);
    CHECK((rows[0].x == 48.0f && rows[0].z == 48.0f && rows[0].threat == 10.0f));
    CHECK((rows[1].x == 112.0f && rows[1].z == 48.0f));
    CHECK((rows[2].x == 48.0f && rows[2].z == 208.0f && rows[2].threat == 5.0f));

    // Along a line: each cell once, summed.
    CHECK_THAT(map.threat_between({40, 0, 40}, {104, 0, 40}, nullptr, ThreatType::Overall, -1),
               WithinAbs(20, 1e-5));
    CHECK_THAT(map.threat_between({40, 0, 40}, {40, 0, 200}, nullptr, ThreatType::Overall, -1),
               WithinAbs(15, 1e-5));

    // The cell of most threat; with none, the cell nearest the start.
    const auto best = map.highest_threat(0, nullptr, ThreatType::Overall, -1, {0, 0, 0});
    CHECK((best.x == 48.0f && best.z == 48.0f && best.threat == 10.0f));
    const auto square = map.highest_threat(1, nullptr, ThreatType::Overall, -1, {0, 0, 0});
    // Cells (2, 0), (2, 1) and (2, 2) each take in both tanks: the nearest
    // to the start wins.
    CHECK((square.x == 80.0f && square.z == 16.0f && square.threat == 20.0f));
    const InfluenceMap empty(512, 512, 2);
    const auto none = empty.highest_threat(0, nullptr, ThreatType::Overall, -1, {300, 0, 90});
    CHECK((none.x == 304.0f && none.z == 80.0f && none.threat == 0.0f));
}

TEST_CASE("Threat types are named as Moho's", "[influence]") {
    ThreatType t = ThreatType::Overall;
    CHECK(sim::threat_type_from_name("AntiSurface", t));
    CHECK(t == ThreatType::AntiSurface);
    CHECK(sim::threat_type_from_name("StructuresNotMex", t));
    CHECK(t == ThreatType::StructuresNotMex);
    CHECK_FALSE(sim::threat_type_from_name("antisurface", t)); // case matters
    CHECK_FALSE(sim::threat_type_from_name("Surface", t));
}
