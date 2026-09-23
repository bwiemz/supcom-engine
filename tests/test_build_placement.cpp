// Structure placement rules behind brain:CanBuildStructureAt and
// brain:FindPlaceToBuild (roadmap M185): the retail AI queues a base by
// asking for a spot, ordering the build, and asking again, so a spot with a
// pending order must read as taken.

#include <catch2/catch_test_macros.hpp>

#include "map/heightmap.hpp"
#include "map/terrain.hpp"
#include "sim/army_brain.hpp"
#include "sim/build_placement.hpp"
#include "sim/manipulator.hpp"
#include "sim/shield.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"

extern "C" {
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

TEST_CASE("Structure placement snaps to the build grid", "[placement]") {
    // Odd footprints center on a cell, even ones on a cell corner, so the
    // footprint covers whole cells -- the ghost and the order agree.
    float x = 10.3f, z = 20.8f;
    osc::sim::snap_structure_center(x, z, 1.0f, 1.0f);
    CHECK(x == 10.5f);
    CHECK(z == 20.5f);

    x = 10.3f; z = 20.8f;
    osc::sim::snap_structure_center(x, z, 2.0f, 4.0f);
    CHECK(x == 10.0f);
    CHECK(z == 20.0f);

    x = 10.9f; z = 20.1f;
    osc::sim::snap_structure_center(x, z, 3.0f, 2.0f);
    CHECK(x == 10.5f);
    CHECK(z == 20.0f);
}
