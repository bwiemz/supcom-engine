// A unit's layer over water, as Moho's CUnitMotion::UpdateCurrentLayer sets
// it (faf-re) each tick it moves: amphibious and land units walk the seabed
// under the water (on the terrain), hover and floating units ride the water
// (at its surface), each from where the ground is under the water's surface
// plus the blueprint's LayerChangeOffsetHeight.

#include <catch2/catch_test_macros.hpp>

#include "map/heightmap.hpp"
#include "map/terrain.hpp"
#include "sim/manipulator.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"
#include "sim/weapon.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using osc::f32;
using osc::sim::CommandType;
using osc::sim::SimState;
using osc::sim::Unit;
using osc::sim::UnitCommand;
using osc::sim::Vector3;

namespace {

struct LuaGuard {
    lua_State* L = lua_open();
    ~LuaGuard() { lua_close(L); }
};

constexpr f32 kWater = 5.0f;
constexpr f32 kLand = 1000.0f / 128.0f; // 7.8

/// 128 x 128: land at 7.8 east of x = 64, a lake bed at 0 west of x = 40,
/// a ramp between, and the water at 5 (so it begins at x = 55.4).
void lake(SimState& sim) {
    std::vector<osc::u16> heights(129 * 129, 1000);
    for (size_t z = 0; z <= 128; ++z)
        for (size_t x = 0; x < 64; ++x)
            heights[z * 129 + x] = x < 40 ? 0 : static_cast<osc::u16>((x - 40) * 1000 / 24);
    osc::map::Heightmap hm(128, 128, 1.0f / 128.0f, std::move(heights));
    sim.set_terrain(std::make_unique<osc::map::Terrain>(std::move(hm), kWater, true));
    sim.build_pathfinding_grid();
    sim.add_army("ARMY_1", "ARMY_1");
    sim.set_fog_of_war("none");
    sim.set_victory_condition("sandbox");
}

Unit* mover(SimState& sim, const std::string& motion_type, f32 x, f32 z) {
    auto u = std::make_unique<Unit>();
    u->set_army(0);
    u->set_max_speed(5.0f);
    u->set_motion_type(motion_type);
    u->set_position({x, kLand, z});
    u->set_max_health(100.0f);
    u->set_health(100.0f);
    Unit::Drive drive;
    drive.max_accel = 50.0f;
    u->set_drive(drive);
    auto* raw = u.get();
    sim.entity_registry().register_entity(std::move(u));
    return raw;
}

UnitCommand move_to(Vector3 at) {
    UnitCommand c;
    c.type = CommandType::Move;
    c.target_pos = at;
    return c;
}

void ticks(SimState& sim, int n) {
    for (int t = 0; t < n; ++t) sim.tick();
}

} // namespace

TEST_CASE("An amphibious unit walks the seabed under the water, and comes back onto land",
          "[seabed]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    lake(sim);
    Unit* u = mover(sim, "RULEUMT_Amphibious", 70.0f, 64.0f);
    u->push_command(move_to({30.0f, 0.0f, 64.0f}), true);
    ticks(sim, 120);
    REQUIRE(std::abs(u->position().x - 30.0f) < 1.0f);
    CHECK(u->layer() == "Seabed");
    CHECK(u->position().y < 0.5f); // on the bed, not the water at 5

    u->push_command(move_to({75.0f, 0.0f, 64.0f}), true);
    ticks(sim, 120);
    REQUIRE(std::abs(u->position().x - 75.0f) < 1.0f);
    CHECK(u->layer() == "Land");
    CHECK(std::abs(u->position().y - kLand) < 0.01f);
}

TEST_CASE("Hover and floating units ride the water's surface on the Water layer", "[seabed]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    lake(sim);
    Unit* hover = mover(sim, "RULEUMT_Hover", 70.0f, 40.0f);
    Unit* floater = mover(sim, "RULEUMT_AmphibiousFloating", 70.0f, 90.0f);
    hover->push_command(move_to({30.0f, 0.0f, 40.0f}), true);
    floater->push_command(move_to({30.0f, 0.0f, 90.0f}), true);
    ticks(sim, 120);
    for (const Unit* u : {hover, floater}) {
        REQUIRE(std::abs(u->position().x - 30.0f) < 1.0f);
        CHECK(u->layer() == "Water");
        CHECK(std::abs(u->position().y - kWater) < 0.01f);
    }

    hover->push_command(move_to({75.0f, 0.0f, 40.0f}), true);
    ticks(sim, 120);
    CHECK(hover->layer() == "Land");
}

TEST_CASE("A unit standing still keeps its layer", "[seabed]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    lake(sim);
    // Put down in the lake (a script's Warp), still: Moho looks again only
    // once it moves.
    Unit* u = mover(sim, "RULEUMT_Amphibious", 30.0f, 64.0f);
    ticks(sim, 5);
    CHECK(u->layer() == "Land");
    u->push_command(move_to({28.0f, 0.0f, 64.0f}), true);
    ticks(sim, 5);
    CHECK(u->layer() == "Seabed");
}

TEST_CASE("LayerChangeOffsetHeight moves where the water begins for a land footprint", "[seabed]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    lake(sim);
    // A deep wader: under water only where the ground is 6 below its surface,
    // never in this lake (5 deep).
    Unit* deep = mover(sim, "RULEUMT_Amphibious", 70.0f, 30.0f);
    deep->set_layer_change_offset(-6.0f);
    deep->push_command(move_to({30.0f, 0.0f, 30.0f}), true);
    // The default, -0.1: under water as soon as the ground is.
    Unit* plain = mover(sim, "RULEUMT_Amphibious", 70.0f, 100.0f);
    plain->push_command(move_to({30.0f, 0.0f, 100.0f}), true);
    ticks(sim, 120);
    CHECK(deep->layer() == "Land");
    CHECK(plain->layer() == "Seabed");
}

TEST_CASE("Where a unit stands: the bed for those that walk it, else the surface", "[seabed]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    lake(sim);
    const auto* terrain = sim.terrain();
    Unit* amph = mover(sim, "RULEUMT_Amphibious", 10.0f, 10.0f);
    Unit* land = mover(sim, "RULEUMT_Land", 10.0f, 20.0f);
    Unit* hover = mover(sim, "RULEUMT_Hover", 10.0f, 30.0f);
    Unit* ship = mover(sim, "RULEUMT_Water", 10.0f, 40.0f);
    ship->set_layer("Water");
    CHECK(amph->ground_y(terrain, 30.0f, 30.0f) == 0.0f);
    CHECK(land->ground_y(terrain, 30.0f, 30.0f) == 0.0f);
    CHECK(hover->ground_y(terrain, 30.0f, 30.0f) == kWater);
    CHECK(ship->ground_y(terrain, 30.0f, 30.0f) == kWater);
    // On land, all the same.
    CHECK(amph->ground_y(terrain, 100.0f, 30.0f) == hover->ground_y(terrain, 100.0f, 30.0f));
}

TEST_CASE("A unit's layer changes on the tick after the step that takes it over the shore",
          "[seabed]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    lake(sim);
    const char* code = "changes = 0 "
                       "return { OnLayerChange = function() changes = changes + 1 end }";
    REQUIRE(luaL_loadbuffer(g.L, code, std::strlen(code), "mover") == 0);
    REQUIRE(lua_pcall(g.L, 0, 1, 0) == 0);
    Unit* u = mover(sim, "RULEUMT_Amphibious", 70.0f, 64.0f);
    u->set_lua_table_ref(luaL_ref(g.L, LUA_REGISTRYINDEX));
    const auto changes = [&] {
        lua_getglobal(g.L, "changes");
        const int n = static_cast<int>(lua_tonumber(g.L, -1));
        lua_pop(g.L, 1);
        return n;
    };
    const auto under = [&](const Vector3& at) {
        const f32 surface = kWater + u->layer_change_offset();
        return sim.terrain()->get_terrain_height(at.x, at.z) < surface;
    };
    u->push_command(move_to({30.0f, 0.0f, 64.0f}), true);
    bool crossed = false;
    for (int t = 0; t < 120 && !crossed; ++t) {
        const bool was_under = under(u->position());
        sim.tick();
        crossed = !was_under && under(u->position());
    }
    REQUIRE(crossed);
    CHECK(u->layer() == "Land");
    CHECK(changes() == 0);
    sim.tick();
    CHECK(u->layer() == "Seabed");
    CHECK(changes() == 1);
}

namespace {

/// Ticks until `u`'s layer is `layer` (at most 200): where it stood on the
/// tick before, and whether it got there.
bool tick_until_layer(SimState& sim, Unit* u, const std::string& layer, Vector3& before) {
    for (int t = 0; t < 200; ++t) {
        before = u->position();
        sim.tick();
        if (u->layer() == layer) return true;
    }
    return false;
}

} // namespace

TEST_CASE("A floating unit settles onto the water over its LayerTransitionDuration",
          "[seabed][layer-transition]") {
    // Moho's CUnitMotion::UpdateCurrentLayer and TransitionBetweenLayers (faf-re): on the tick a
    // floating unit's layer becomes Water, its motion blends it from where it stood to the
    // water's surface under it (SnapToWater: the higher of the water and 0.25 over the ground),
    // upright, over the duration's ticks (the seconds times 10), and moves it no other way
    // meanwhile; its velocity is kept. The Salem (URS0201) takes 4 s.
    LuaGuard g;
    SimState sim(g.L, nullptr);
    lake(sim);
    Unit* slow = mover(sim, "RULEUMT_AmphibiousFloating", 70.0f, 40.0f);
    slow->set_layer_transition_duration(4.0f);
    Unit* plain = mover(sim, "RULEUMT_AmphibiousFloating", 70.0f, 90.0f);
    slow->push_command(move_to({30.0f, 0.0f, 40.0f}), true);
    plain->push_command(move_to({30.0f, 0.0f, 90.0f}), true);
    // Each tick, when each reaches the far side
    osc::u32 slow_at = 0, plain_at = 0;
    const auto step = [&] {
        sim.tick();
        if (!slow_at && std::abs(slow->position().x - 30.0f) < 1.0f) slow_at = sim.tick_count();
        if (!plain_at && std::abs(plain->position().x - 30.0f) < 1.0f) plain_at = sim.tick_count();
    };

    Vector3 from;
    for (int t = 0; t < 200 && slow->layer() != "Water"; ++t) {
        from = slow->position();
        step();
    }
    REQUIRE(slow->layer() == "Water");
    REQUIRE(slow->in_layer_transition());
    const f32 ground = sim.terrain()->get_terrain_height(from.x, from.z);
    const f32 to_y = std::max(ground + 0.25f, kWater);
    const Vector3 kept = slow->velocity();
    CHECK(kept.x < -1.0f); // still heading west
    // The first of 39 blend ticks ran with the layer change: 38 more
    for (int k = 1; k <= 39; ++k) {
        INFO("blend tick " << k);
        REQUIRE(slow->in_layer_transition() == (k < 39));
        CHECK(slow->position().x == from.x);
        CHECK(slow->position().z == from.z);
        CHECK(std::abs(slow->position().y - (from.y + (to_y - from.y) * k / 40.0f)) < 1e-4f);
        CHECK(slow->velocity().x == kept.x);
        if (k < 39) step();
    }
    CHECK(osc::sim::quat_rotate(slow->orientation(), {0.0f, 1.0f, 0.0f}).y > 0.9999f);

    // Then it goes on, and arrives the crossing's 39 ticks or so after the other
    for (int t = 0; t < 400 && (!slow_at || !plain_at); ++t) step();
    REQUIRE(slow_at != 0);
    REQUIRE(plain_at != 0);
    CHECK(slow->layer() == "Water");
    CHECK(std::abs(slow->position().y - kWater) < 0.01f);
    const int late = static_cast<int>(slow_at) - static_cast<int>(plain_at);
    CHECK(late >= 34);
    CHECK(late <= 44);
}

TEST_CASE("A floating unit climbs onto the land it is heading for over its LayerTransitionDuration",
          "[seabed][layer-transition]") {
    // Leaving the water, the blend ends on the ground ten ticks of its velocity ahead
    // (SnapToGround there), so it moves on up the shore as it climbs.
    LuaGuard g;
    SimState sim(g.L, nullptr);
    lake(sim);
    Unit* u = mover(sim, "RULEUMT_AmphibiousFloating", 30.0f, 64.0f);
    u->set_layer("Water");
    u->set_position({30.0f, kWater, 64.0f});
    u->set_layer_transition_duration(2.0f);
    u->push_command(move_to({80.0f, 0.0f, 64.0f}), true);

    Vector3 from;
    REQUIRE(tick_until_layer(sim, u, "Land", from));
    REQUIRE(u->in_layer_transition());
    const Vector3 v = u->velocity();
    REQUIRE(v.x > 1.0f);
    const f32 lead_x = from.x + v.x * 1.0f; // 10 ticks of 0.1 s
    f32 last_x = u->position().x;
    for (int k = 1; k <= 19; ++k) {
        INFO("blend tick " << k);
        REQUIRE(u->in_layer_transition() == (k < 19));
        CHECK(std::abs(u->position().x - (from.x + (lead_x - from.x) * k / 20.0f)) < 1e-4f);
        CHECK(u->position().x >= last_x);
        last_x = u->position().x;
        if (k < 19) sim.tick();
    }
    // On the ramp's ground there, nearly
    const f32 lead_ground = sim.terrain()->get_terrain_height(lead_x, from.z);
    CHECK(std::abs(u->position().y - (from.y + (lead_ground - from.y) * 19 / 20.0f)) < 0.05f);
    CHECK(u->layer() == "Land");
}

TEST_CASE("Without a LayerTransitionDuration a floating unit crosses the shore at once",
          "[seabed][layer-transition]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    lake(sim);
    Unit* u = mover(sim, "RULEUMT_AmphibiousFloating", 70.0f, 64.0f);
    u->push_command(move_to({30.0f, 0.0f, 64.0f}), true);
    Vector3 from;
    REQUIRE(tick_until_layer(sim, u, "Water", from));
    CHECK_FALSE(u->in_layer_transition());
    CHECK(u->position().x != from.x); // it moved on the same tick
}
