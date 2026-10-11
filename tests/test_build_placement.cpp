// Structure placement rules behind brain:CanBuildStructureAt and
// brain:FindPlaceToBuild (roadmap M185): the retail AI queues a base by
// asking for a spot, ordering the build, and asking again, so a spot with a
// pending order must read as taken.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "map/heightmap.hpp"
#include "map/terrain.hpp"
#include "renderer/input_handler.hpp"
#include "renderer/renderer.hpp"
#include "sim/army_brain.hpp"
#include "sim/build_placement.hpp"
#include "sim/manipulator.hpp"
#include "sim/shield.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"

extern "C" {
#include <cmath>
#include <lua.h>
}

#include <memory>
#include <string>
#include <vector>

using osc::sim::PlacementRules;
using osc::sim::SimState;
using osc::sim::StructurePlacement;
using osc::sim::Unit;

namespace {

struct LuaGuard {
    lua_State* L = lua_open();
    ~LuaGuard() { lua_close(L); }
};

constexpr osc::u32 kMapSize = 128;

/// 128x128 map: dry land for x < 64, sea (water elevation above ground) beyond.
void make_coast_world(SimState& sim) {
    std::vector<osc::u16> heights((kMapSize + 1) * (kMapSize + 1), 1000);
    for (osc::u32 z = 0; z <= kMapSize; ++z)
        for (osc::u32 x = 64; x <= kMapSize; ++x)
            heights[z * (kMapSize + 1) + x] = 100; // ~0.8 units, under the sea
    osc::map::Heightmap hm(kMapSize, kMapSize, 1.0f / 128.0f, std::move(heights));
    sim.set_terrain(std::make_unique<osc::map::Terrain>(std::move(hm), 5.0f, true));
    sim.build_pathfinding_grid();
}

PlacementRules rules_for(const std::string& bp) {
    PlacementRules r;
    if (bp == "pgen") {
        r.size_x = r.size_z = 2.0f;
    } else if (bp == "factory") {
        r.size_x = r.size_z = 8.0f;
    } else if (bp == "mex") {
        r.size_x = r.size_z = 2.0f;
        r.deposit = PlacementRules::Deposit::Mass;
    } else if (bp == "seafactory") {
        r.size_x = r.size_z = 8.0f;
        r.on_land = false;
        r.on_water = true;
    }
    return r;
}

Unit* spawn(SimState& sim, osc::i32 army, osc::f32 x, osc::f32 z) {
    auto u = std::make_unique<Unit>();
    u->set_army(army);
    u->set_position({x, 0.0f, z});
    u->add_category("ENGINEER");
    auto* raw = u.get();
    sim.entity_registry().register_entity(std::move(u));
    return raw;
}

void order_build(Unit* builder, const std::string& bp, osc::f32 x, osc::f32 z) {
    osc::sim::UnitCommand cmd;
    cmd.type = osc::sim::CommandType::BuildMobile;
    cmd.target_pos = {x, 0.0f, z};
    cmd.blueprint_id = bp;
    builder->push_command(cmd, false);
}

} // namespace

TEST_CASE("placement: terrain layer and map bounds", "[placement]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    make_coast_world(sim);
    StructurePlacement p(sim, 0, rules_for);

    CHECK(p.can_build("pgen", 20.0f, 20.0f));
    CHECK_FALSE(p.can_build("pgen", 100.0f, 20.0f));       // land-only, at sea
    CHECK(p.can_build("seafactory", 100.0f, 20.0f));       // naval, at sea
    CHECK_FALSE(p.can_build("seafactory", 20.0f, 20.0f));  // naval, on land
    CHECK_FALSE(p.can_build("factory", 2.0f, 20.0f));      // hangs off the map
}

TEST_CASE("placement: structures block, edge contact is allowed", "[placement]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    make_coast_world(sim);
    auto* pgen = spawn(sim, 1, 20.0f, 20.0f); // any army's structure blocks
    pgen->add_category("STRUCTURE");
    pgen->set_footprint_size(2.0f, 2.0f);
    pgen->set_is_being_built(true);           // under construction counts
    StructurePlacement p(sim, 0, rules_for);

    CHECK_FALSE(p.can_build("pgen", 20.0f, 20.0f));
    CHECK_FALSE(p.can_build("pgen", 21.0f, 20.0f)); // overlaps by half
    CHECK(p.can_build("pgen", 22.0f, 20.0f));       // shares an edge
    CHECK_FALSE(p.can_build("factory", 24.0f, 20.0f));
}

TEST_CASE("placement: pending build orders reserve their site for the army",
          "[placement]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    make_coast_world(sim);
    auto* engineer = spawn(sim, 0, 10.0f, 10.0f);
    order_build(engineer, "factory", 30.0f, 30.0f);
    order_build(engineer, "pgen", 40.0f, 30.0f);

    StructurePlacement mine(sim, 0, rules_for);
    CHECK_FALSE(mine.can_build("pgen", 30.0f, 30.0f)); // inside the queued factory
    CHECK_FALSE(mine.can_build("pgen", 40.0f, 30.0f)); // the queued pgen itself
    CHECK(mine.can_build("pgen", 42.0f, 30.0f));

    StructurePlacement theirs(sim, 1, rules_for); // another army's plans
    CHECK(theirs.can_build("pgen", 30.0f, 30.0f));
}

TEST_CASE("placement: a build order being moved doesn't block its own new site", "[placement]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    make_coast_world(sim);
    auto* engineer = spawn(sim, 0, 10.0f, 10.0f);
    osc::sim::UnitCommand cmd;
    cmd.type = osc::sim::CommandType::BuildMobile;
    cmd.target_pos = {30.0f, 0.0f, 30.0f};
    cmd.blueprint_id = "factory";
    cmd.command_id = 6;
    engineer->push_command(cmd, false);
    CHECK_FALSE(StructurePlacement(sim, 0, rules_for).can_build("factory", 32.0f, 30.0f));
    CHECK(StructurePlacement(sim, 0, rules_for, false, 6).can_build("factory", 32.0f, 30.0f));
    CHECK_FALSE(StructurePlacement(sim, 0, rules_for, false, 7).can_build("factory", 32.0f, 30.0f));
}

TEST_CASE("placement: extractors need a free deposit", "[placement]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    make_coast_world(sim);
    osc::sim::ResourceDeposit mass;
    mass.x = 30.0f;
    mass.z = 40.0f;
    sim.add_resource_deposit(mass);

    StructurePlacement p(sim, 0, rules_for);
    CHECK(p.can_build("mex", 30.0f, 40.0f));
    CHECK_FALSE(p.can_build("mex", 50.0f, 40.0f)); // no deposit there
    CHECK(p.can_build("pgen", 50.0f, 40.0f));      // non-extractors don't care

    auto* mex = spawn(sim, 0, 30.0f, 40.0f);
    mex->add_category("STRUCTURE");
    mex->set_footprint_size(2.0f, 2.0f);
    StructurePlacement after(sim, 0, rules_for);
    CHECK_FALSE(after.can_build("mex", 30.0f, 40.0f)); // deposit taken
}

TEST_CASE("placement: an extractor goes on its deposit's own cell", "[placement]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    make_coast_world(sim);
    osc::sim::ResourceDeposit mass;
    mass.x = 30.5f;
    mass.z = 40.5f;
    sim.add_resource_deposit(mass);
    const auto rules = [](const std::string& bp) {
        PlacementRules r = rules_for(bp);
        if (bp == "mex") {
            r.size_x = r.size_z = 1.0f;
        }
        return r;
    };

    StructurePlacement p(sim, 0, rules);
    CHECK(p.can_build("mex", 30.5f, 40.5f));
    CHECK_FALSE(p.can_build("mex", 31.5f, 40.5f));
    CHECK_FALSE(p.can_build("mex", 29.5f, 39.5f));
}

TEST_CASE("placement: a structure's skirt is its pad", "[placement]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    make_coast_world(sim);
    // An air factory's: footprint 5, skirt 8 from 1.5 outside it
    auto* factory = spawn(sim, 0, 30.5f, 30.5f);
    factory->add_category("STRUCTURE");
    factory->set_footprint_size(5.0f, 5.0f);
    factory->set_skirt(8.0f, 8.0f, -1.5f, -1.5f);
    // A T1 extractor's or power generator's: footprint 1, skirt 2
    const auto rules = [](const std::string& bp) {
        PlacementRules r = rules_for(bp);
        if (bp == "pgen") {
            r.size_x = r.size_z = 1.0f;
            r.skirt_x = r.skirt_z = 2.0f;
            r.skirt_off_x = r.skirt_off_z = -0.5f;
        }
        return r;
    };

    StructurePlacement p(sim, 0, rules);
    CHECK_FALSE(p.can_build("pgen", 30.5f, 34.5f));
    CHECK(p.can_build("pgen", 30.5f, 35.5f));
    auto* engineer = spawn(sim, 0, 10.0f, 10.0f);
    int along = 0;
    for (int i = 0; i < 7; ++i) {
        const osc::f32 x = 27.5f + static_cast<osc::f32>(i);
        if (StructurePlacement(sim, 0, rules).can_build("pgen", x, 35.5f)) {
            order_build(engineer, "pgen", x, 35.5f);
            ++along;
        }
    }
    CHECK(along == 4);
}

TEST_CASE("placement: an order not yet in a queue reserves its site", "[placement]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    make_coast_world(sim);
    auto* engineer = spawn(sim, 0, 10.0f, 10.0f);
    osc::sim::ScheduledCommand order;
    order.exec_tick = sim.tick_count() + 1;
    order.unit_ids = {engineer->entity_id()};
    order.command.type = osc::sim::CommandType::BuildMobile;
    order.command.target_pos = {30.0f, 0.0f, 30.0f};
    order.command.blueprint_id = "pgen";
    sim.command_scheduler().submit(order);

    CHECK_FALSE(StructurePlacement(sim, 0, rules_for, true).can_build("pgen", 30.0f, 30.0f));
    CHECK(StructurePlacement(sim, 1, rules_for, true).can_build("pgen", 30.0f, 30.0f));
    CHECK(StructurePlacement(sim, 0, rules_for).can_build("pgen", 30.0f, 30.0f));
}

TEST_CASE("placement: an order not yet run that replaces a queue frees its sites", "[placement]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    make_coast_world(sim);
    auto* engineer = spawn(sim, 0, 10.0f, 10.0f);
    order_build(engineer, "pgen", 30.0f, 30.0f);
    osc::sim::UnitCommand cmd;
    cmd.type = osc::sim::CommandType::BuildMobile;
    cmd.target_pos = {40.0f, 0.0f, 30.0f};
    cmd.blueprint_id = "pgen";
    sim.schedule_command(0, {engineer->entity_id()}, cmd, true);

    StructurePlacement p(sim, 0, rules_for, true);
    CHECK(p.can_build("pgen", 30.0f, 30.0f));
    CHECK_FALSE(p.can_build("pgen", 40.0f, 30.0f));
}

TEST_CASE("placement: a seabed structure goes on the ground under the sea", "[placement]") {
    // An extractor's BuildOnLayerCaps give LAYER_Land and LAYER_Seabed: it
    // stands on dry land or on the sea floor, never afloat on its own.
    LuaGuard g;
    SimState sim(g.L, nullptr);
    make_coast_world(sim);
    const auto rules = [](const std::string& bp) {
        PlacementRules r = rules_for(bp);
        if (bp == "seabed") {
            r.size_x = r.size_z = 2.0f;
            r.on_seabed = true;
        }
        return r;
    };
    StructurePlacement p(sim, 0, rules);
    CHECK(p.can_build("seabed", 100.0f, 20.0f)); // under the sea
    CHECK(p.can_build("seabed", 20.0f, 20.0f));  // on land
    CHECK_FALSE(p.can_build("pgen", 100.0f, 20.0f));
}

TEST_CASE("placement: a ghost stands where its structure would", "[placement]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    make_coast_world(sim);
    osc::renderer::InputHandler input;
    osc::renderer::CommandModeHooks hooks;
    hooks.can_place = [&sim](osc::i32, const std::string& bp, osc::f32, osc::f32, osc::u32) {
        sim.placement_rules(bp, [&bp] { return rules_for(bp); });
        return true;
    };
    input.set_command_mode_hooks(std::move(hooks));
    sim.set_build_ghost("seafactory", 8.0f, 8.0f);
    CHECK(input.ghost_at(sim, 100.0f, 20.0f).y == Catch::Approx(5.0f));
    sim.set_build_ghost("pgen", 2.0f, 2.0f);
    CHECK(input.ghost_at(sim, 20.0f, 20.0f).y ==
          Catch::Approx(sim.terrain()->get_terrain_height(20.0f, 20.0f)));
}

TEST_CASE("placement: a build template shows a ghost of each structure, its own blueprint "
          "judged on its own, none off the playable area",
          "[placement][build_template]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    make_coast_world(sim); // land for x < 64, sea beyond
    osc::renderer::InputHandler input;
    osc::renderer::CommandModeHooks hooks;
    hooks.can_place = [&sim](osc::i32 army, const std::string& bp, osc::f32 x, osc::f32 z,
                             osc::u32) {
        sim.placement_rules(bp, [&bp] { return rules_for(bp); });
        return StructurePlacement(sim, army, rules_for).can_build(bp, x, z);
    };
    hooks.footprint = [](const std::string& bp) {
        const PlacementRules r = rules_for(bp);
        return std::array<osc::f32, 2>{r.size_x, r.size_z};
    };
    input.set_command_mode_hooks(std::move(hooks));
    // A generator, a factory 10 east of it, a generator 30 east (in the sea)
    auto& t = input.build_template();
    t.span_x = 40;
    t.span_z = 8;
    t.entries = {{"pgen", 1, 0, 0}, {"factory", 2, 10, 0}, {"pgen", 3, 30, 0}};
    osc::renderer::CommandMode mode;
    mode.mode = "build";
    mode.name = "pgen";
    REQUIRE(input.placing_template(mode));
    // Lead 2x2: corner cell (lrint(39.3), lrint(19.2)) = (39, 19), so the
    // anchor is (39.5, 19.5) and the generator snaps to (39, 19) --
    // nearbyint(38.5) and nearbyint(18.5) round to even, + 1 -- the 8x8
    // factory to (50, 20), the far generator to (69, 19).
    const auto ghost = input.template_ghost(sim, mode, 40.3f, 20.2f, 40.3f, 20.2f);
    REQUIRE(ghost);
    CHECK(ghost->blueprint_id == "pgen");
    CHECK(ghost->x == 39.0f);
    CHECK(ghost->z == 19.0f);
    CHECK(ghost->valid);
    REQUIRE(ghost->line.size() == 2);
    CHECK(ghost->line[0].blueprint_id == "factory");
    CHECK(ghost->line[0].x == 50.0f);
    CHECK(ghost->line[0].z == 20.0f);
    CHECK(ghost->line[0].valid);
    CHECK(ghost->line[1].blueprint_id == "pgen");
    CHECK(ghost->line[1].x == 69.0f);
    CHECK_FALSE(ghost->line[1].valid); // in the sea, the rest still valid

    // The far generator off the playable area: no ghost for it
    sim.set_playable_rect(0, 0, 60, 128);
    const auto inside = input.template_ghost(sim, mode, 40.3f, 20.2f, 40.3f, 20.2f);
    REQUIRE(inside);
    CHECK(inside->line.size() == 1);
    input.build_template() = {};
    CHECK_FALSE(input.placing_template(mode));
}

TEST_CASE("Structure placement snaps to the build grid", "[placement]") {
    float x = 10.3f, z = 20.8f;
    osc::sim::snap_structure_center(x, z, 1.0f, 1.0f);
    CHECK(x == 10.5f);
    CHECK(z == 20.5f);

    x = 10.3f; z = 20.8f;
    osc::sim::snap_structure_center(x, z, 2.0f, 4.0f);
    CHECK(x == 10.0f);
    CHECK(z == 21.0f);

    x = 10.9f; z = 20.1f;
    osc::sim::snap_structure_center(x, z, 3.0f, 2.0f);
    CHECK(x == 10.5f);
    CHECK(z == 20.0f);
}

TEST_CASE("A build drag lays its structure along the drag", "[placement]") {
    using osc::sim::structure_line_sites;
    using Sites = std::vector<std::pair<osc::f32, osc::f32>>;
    CHECK(structure_line_sites(10.3f, 20.7f, 10.3f, 20.7f, 1, 1, 1) == Sites{{10.5f, 20.5f}});
    CHECK(structure_line_sites(10.3f, 20.7f, 15.6f, 20.2f, 1, 1, 1) == Sites{{10.5f, 20.5f},
                                                                             {11.5f, 20.5f},
                                                                             {12.5f, 20.5f},
                                                                             {13.5f, 20.5f},
                                                                             {14.5f, 20.5f},
                                                                             {15.5f, 20.5f}});
    CHECK(structure_line_sites(0.5f, 0.5f, 6.5f, 0.5f, 1, 1, 2) ==
          Sites{{0.5f, 0.5f}, {2.5f, 0.5f}, {4.5f, 0.5f}, {6.5f, 0.5f}});
    CHECK(structure_line_sites(1, 1, 11, 1, 2, 2, 4) == Sites{{1, 1}, {5, 1}, {9, 1}});
    CHECK(structure_line_sites(0.5f, 0.5f, 4.5f, 2.5f, 1, 1, 1) ==
          Sites{{0.5f, 0.5f}, {1.5f, 0.5f}, {2.5f, 1.5f}, {3.5f, 2.5f}, {4.5f, 2.5f}});
    CHECK(structure_line_sites(0.5f, 4.5f, 0.5f, 0.5f, 1, 1, 2) ==
          Sites{{0.5f, 4.5f}, {0.5f, 2.5f}, {0.5f, 0.5f}});
}

TEST_CASE("placement: a blueprint's rules are read once per game", "[placement]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    int reads = 0;
    const auto read = [&] {
        ++reads;
        return rules_for("factory");
    };
    CHECK(sim.placement_rules("factory", read).size_x == rules_for("factory").size_x);
    CHECK(sim.placement_rules("factory", read).size_x == rules_for("factory").size_x);
    CHECK(reads == 1);
}

TEST_CASE("placement: pending orders are gathered only for a site the terrain allows",
          "[placement]") {
    // Walking every unit's queue is most of a query's cost; a site off the
    // map or on the wrong layer is refused before it.
    LuaGuard g;
    SimState sim(g.L, nullptr);
    make_coast_world(sim);
    auto* engineer = spawn(sim, 0, 10.0f, 10.0f);
    order_build(engineer, "factory", 30.0f, 30.0f);

    std::vector<std::string> looked_up;
    StructurePlacement p(sim, 0, [&](const std::string& bp) {
        looked_up.push_back(bp);
        return rules_for(bp);
    });
    CHECK(looked_up.empty());
    CHECK_FALSE(p.can_build("pgen", 100.0f, 20.0f)); // at sea: the terrain refuses it
    CHECK(looked_up == std::vector<std::string>{"pgen"});
    CHECK_FALSE(p.can_build("pgen", 30.0f, 30.0f)); // on land, inside the queued factory
    CHECK(looked_up == std::vector<std::string>{"pgen", "factory"});
    CHECK(p.can_build("pgen", 42.0f, 30.0f));
    CHECK(looked_up.size() == 2); // gathered once
}

TEST_CASE("An extractor's cursor snaps to the nearest deposit within reach", "[placement]") {
    using osc::sim::PlacementRules;
    using osc::sim::ResourceDeposit;
    using Deposit = PlacementRules::Deposit;
    std::vector<ResourceDeposit> deposits;
    deposits.push_back({10.5f, 0, 20.5f, 1.0f, ResourceDeposit::Mass});
    deposits.push_back({13.5f, 0, 20.5f, 1.0f, ResourceDeposit::Hydrocarbon});
    deposits.push_back({30.5f, 0, 30.5f, 3.0f, ResourceDeposit::Hydrocarbon});
    deposits.push_back({16.5f, 0, 20.5f, 1.0f, ResourceDeposit::Mass});

    const auto at = osc::sim::deposit_snap(deposits, Deposit::Mass, 12.7f, 19.2f, 1, 1, 4);
    REQUIRE(at);
    CHECK(at->first == Catch::Approx(10.5f));
    CHECK(at->second == Catch::Approx(20.5f));

    const auto nearer = osc::sim::deposit_snap(deposits, Deposit::Mass, 14.6f, 20.5f, 1, 1, 4);
    REQUIRE(nearer);
    CHECK(nearer->first == Catch::Approx(16.5f));

    CHECK_FALSE(osc::sim::deposit_snap(deposits, Deposit::Mass, 10.5f, 26.5f, 1, 1, 4));
    CHECK_FALSE(osc::sim::deposit_snap(deposits, Deposit::None, 10.5f, 20.5f, 1, 1, 4));

    const auto hydro =
        osc::sim::deposit_snap(deposits, Deposit::Hydrocarbon, 32.2f, 29.1f, 3, 3, 4);
    REQUIRE(hydro);
    CHECK(hydro->first == Catch::Approx(30.5f));
    CHECK(hydro->second == Catch::Approx(30.5f));

    CHECK(osc::sim::extract_snap_radius(0.1f) == Catch::Approx(4.0f));
    CHECK(osc::sim::extract_snap_radius(0.01f) == Catch::Approx(0.9f));
    CHECK(osc::sim::extract_snap_radius(1.0f) == Catch::Approx(20.0f));
}

namespace {

/// 128x128 map of dry land at ~7.8, with a plateau ~2.3 higher for
/// 40 <= x < 60, and sea (water 5.0) beyond x = 96 at depth `sea_floor`.
void make_step_world(SimState& sim, osc::u16 sea_floor = 100) {
    std::vector<osc::u16> heights((kMapSize + 1) * (kMapSize + 1), 1000);
    for (osc::u32 z = 0; z <= kMapSize; ++z) {
        for (osc::u32 x = 40; x < 60; ++x) heights[z * (kMapSize + 1) + x] = 1300;
        for (osc::u32 x = 96; x <= kMapSize; ++x) heights[z * (kMapSize + 1) + x] = sea_floor;
    }
    osc::map::Heightmap hm(kMapSize, kMapSize, 1.0f / 128.0f, std::move(heights));
    sim.set_terrain(std::make_unique<osc::map::Terrain>(std::move(hm), 5.0f, true));
    sim.build_pathfinding_grid();
}

} // namespace

TEST_CASE("placement: a structure's skirt must be flat within MaxGroundVariation", "[placement]") {
    // Moho's OCCUPY_CheckAreaFlatness: every height point under the skirt
    // within MaxGroundVariation (default 1.0) of the others.
    LuaGuard g;
    SimState sim(g.L, nullptr);
    make_step_world(sim);
    const auto rules = [](const std::string& bp) {
        PlacementRules r = rules_for(bp);
        if (bp == "wall") {
            r.size_x = r.size_z = 2.0f;
            r.max_ground_variation = 50.0f; // walls take any slope
        }
        return r;
    };
    StructurePlacement p(sim, 0, rules);
    CHECK(p.can_build("pgen", 20.0f, 20.0f));       // flat
    CHECK(p.can_build("pgen", 50.0f, 20.0f));       // on the plateau, flat
    CHECK_FALSE(p.can_build("pgen", 40.0f, 20.0f)); // across its 2.3 step
    CHECK(p.can_build("wall", 40.0f, 20.0f));
}

TEST_CASE("placement: a FlattenSkirt structure needs only its skirt's edge near its level",
          "[placement]") {
    // OCCUPY_CheckEdgeFlatness: the ring just outside the skirt, against the
    // ceiling of its lowest point, the level the ground is cut to.
    LuaGuard g;
    SimState sim(g.L, nullptr);
    std::vector<osc::u16> heights((kMapSize + 1) * (kMapSize + 1), 1024); // 8.0
    // A 2.3 bump at (20, 20), inside a 4x4 skirt centred there
    heights[20 * (kMapSize + 1) + 20] = 1324;
    osc::map::Heightmap hm(kMapSize, kMapSize, 1.0f / 128.0f, std::move(heights));
    sim.set_terrain(std::make_unique<osc::map::Terrain>(std::move(hm), 0.0f, false));
    sim.build_pathfinding_grid();
    const auto rules = [](const std::string& bp) {
        PlacementRules r;
        r.size_x = r.size_z = 4.0f;
        r.flatten_skirt = bp == "flattened";
        return r;
    };
    const osc::map::Terrain& t = *sim.terrain();
    CHECK(osc::sim::occupy_layers(t, rules("plain"), 20.0f, 20.0f) == 0);
    CHECK(osc::sim::occupy_layers(t, rules("flattened"), 20.0f, 20.0f) ==
          osc::sim::placement_layer::Land);
}

TEST_CASE("placement: a FlattenSkirt structure goes on a gentle slope", "[placement]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    std::vector<osc::u16> heights((kMapSize + 1) * (kMapSize + 1));
    for (osc::u32 z = 0; z <= kMapSize; ++z) {
        for (osc::u32 x = 0; x <= kMapSize; ++x) {
            const float h = 20.25f + 0.06f * (static_cast<float>(x) - 20.0f) -
                            0.06f * (static_cast<float>(z) - 20.0f);
            heights[z * (kMapSize + 1) + x] = static_cast<osc::u16>(std::lround(h * 128.0f));
        }
    }
    osc::map::Heightmap hm(kMapSize, kMapSize, 1.0f / 128.0f, std::move(heights));
    sim.set_terrain(std::make_unique<osc::map::Terrain>(std::move(hm), 0.0f, false));
    PlacementRules r;
    r.size_x = r.size_z = 4.0f;
    r.flatten_skirt = true;
    CHECK(osc::sim::occupy_layers(*sim.terrain(), r, 20.0f, 20.0f) ==
          osc::sim::placement_layer::Land);
}

TEST_CASE("placement: a structure in the water needs MinWaterDepth over its skirt", "[placement]") {
    // Moho drops Water, Sub and Seabed unless the skirt's highest point is
    // MinWaterDepth under the surface; Land needs it all above the water.
    LuaGuard g;
    SimState sim(g.L, nullptr);
    make_step_world(sim); // sea floor ~0.8: 4.2 deep
    const auto rules = [](const std::string& bp) {
        PlacementRules r = rules_for(bp);
        if (bp == "deepfactory") {
            r = rules_for("seafactory");
            r.min_water_depth = 5.0f;
        } else if (bp == "shallowfactory") {
            r = rules_for("seafactory");
            r.min_water_depth = 1.5f;
        }
        return r;
    };
    StructurePlacement p(sim, 0, rules);
    CHECK(p.can_build("shallowfactory", 110.0f, 20.0f));
    CHECK_FALSE(p.can_build("deepfactory", 110.0f, 20.0f));
    const osc::map::Terrain& t = *sim.terrain();
    // A land structure with a point of its skirt under the water: no land
    CHECK(osc::sim::occupy_layers(t, rules("pgen"), 96.0f, 20.0f) == 0);
}

TEST_CASE("placement: a structure built only under the surface goes in deep water", "[placement]") {
    // The Cybran HARMS: BuildOnLayerCaps LAYER_Sub alone, MinWaterDepth 2.
    LuaGuard g;
    SimState sim(g.L, nullptr);
    make_step_world(sim);
    const auto rules = [](const std::string& bp) {
        PlacementRules r = rules_for(bp);
        if (bp == "harms") {
            r.size_x = r.size_z = 2.0f;
            r.skirt_x = r.skirt_z = 3.0f;
            r.on_land = false;
            r.on_sub = true;
            r.min_water_depth = 2.0f;
            r.flatten_skirt = true;
        }
        return r;
    };
    StructurePlacement p(sim, 0, rules);
    CHECK(p.can_build("harms", 110.0f, 20.0f));
    CHECK_FALSE(p.can_build("harms", 20.0f, 20.0f));

    LuaGuard g2;
    SimState shallow(g2.L, nullptr);
    make_step_world(shallow, 500); // sea floor ~3.9: 1.1 deep
    StructurePlacement q(shallow, 0, rules);
    CHECK_FALSE(q.can_build("harms", 110.0f, 20.0f));
}
