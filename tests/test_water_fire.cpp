// A weapon that fires only above the water (AboveWaterFireOnly) asks where
// its muzzle is against the water's surface, not which layer its unit is on
// (Moho's UnitWeapon fire check, faf-re): a Monkeylord standing on the
// seabed fires while its guns are above the water.

#include <catch2/catch_test_macros.hpp>

#include "map/heightmap.hpp"
#include "map/terrain.hpp"
#include "sim/manipulator.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/weapon.hpp"

extern "C" {
#include <lua.h>
}

#include <memory>
#include <vector>

using osc::f32;
using osc::sim::SimState;
using osc::sim::Unit;
using osc::sim::Weapon;

namespace {

struct LuaGuard {
    lua_State* L = lua_open();
    ~LuaGuard() { lua_close(L); }
};

/// Ground at 7.8125 everywhere, its water's surface at `water`.
void ground_under(SimState& sim, f32 water) {
    std::vector<osc::u16> heights(129 * 129, 1000);
    osc::map::Heightmap hm(128, 128, 1.0f / 128.0f, std::move(heights));
    sim.set_terrain(std::make_unique<osc::map::Terrain>(std::move(hm), water, true));
    sim.add_army("ARMY_1", "ARMY_1");
    sim.add_army("ARMY_2", "ARMY_2");
    sim.set_fog_of_war("none");
}

Unit* unit_at(SimState& sim, osc::i32 army, f32 x, f32 y, f32 z) {
    auto u = std::make_unique<Unit>();
    u->set_army(army);
    u->set_motion_type("RULEUMT_Amphibious");
    u->set_position({x, y, z});
    u->set_max_health(100.0f);
    u->set_health(100.0f);
    auto* raw = u.get();
    sim.entity_registry().register_entity(std::move(u));
    return raw;
}

/// Whether a gun of `owner`'s, AboveWaterFireOnly or not, can fire at
/// `target` (no muzzle bone: its muzzle is the unit's position).
bool fires(SimState& sim, Unit& owner, const Unit& target, bool above_water_only) {
    auto gun = std::make_unique<Weapon>();
    gun->max_range = 40.0f;
    gun->above_water_fire_only = above_water_only;
    gun->set_target_entity(target.entity_id());
    return gun->can_fire(owner, sim.entity_registry(), &sim);
}

} // namespace

TEST_CASE("AboveWaterFireOnly: the muzzle above the water fires, below it doesn't",
          "[weapon][water]") {
    for (const char* layer : {"Land", "Seabed"}) {
        INFO("the unit's layer: " << layer);
        {
            // Water 2.2 over the ground: the muzzle, at the unit, is under it
            LuaGuard g;
            SimState sim(g.L, nullptr);
            ground_under(sim, 10.0f);
            Unit* walker = unit_at(sim, 0, 40.0f, 7.8125f, 40.0f);
            walker->set_layer(layer);
            const Unit* target = unit_at(sim, 1, 60.0f, 7.8125f, 40.0f);
            CHECK_FALSE(fires(sim, *walker, *target, true));
            CHECK(fires(sim, *walker, *target, false));
        }
        {
            // The same muzzle, the water now below it
            LuaGuard g;
            SimState sim(g.L, nullptr);
            ground_under(sim, 5.0f);
            Unit* walker = unit_at(sim, 0, 40.0f, 7.8125f, 40.0f);
            walker->set_layer(layer);
            const Unit* target = unit_at(sim, 1, 60.0f, 7.8125f, 40.0f);
            CHECK(fires(sim, *walker, *target, true));
        }
    }
}
