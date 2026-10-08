// A jammer's fake blips: when an army knows one fake (Moho's
// CAiReconDBImpl::UpdateBlip for a fake). Sight and omni are the intel
// tests' (jammer_blip_test); this is the playable area's edge.

#include <catch2/catch_test_macros.hpp>

#include "blueprints/footprint.hpp"
#include "map/heightmap.hpp"
#include "map/terrain.hpp"
#include "sim/army_brain.hpp"
#include "sim/manipulator.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"

extern "C" {
#include <lua.h>
}

#include <memory>
#include <vector>

using osc::sim::SimState;
using osc::sim::Unit;
using osc::sim::Vector3;

namespace {

struct LuaGuard {
    lua_State* L = lua_open();
    ~LuaGuard() { lua_close(L); }
};

} // namespace

TEST_CASE("A jammer's fake is known fake outside the playable area inset by the jammer's larger "
          "footprint side",
          "[jammer]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    constexpr osc::u32 kMap = 128;
    std::vector<osc::u16> heights(static_cast<size_t>(kMap + 1) * (kMap + 1), 1000);
    osc::map::Heightmap hm(kMap, kMap, 1.0f / 128.0f, std::move(heights));
    sim.set_terrain(std::make_unique<osc::map::Terrain>(std::move(hm), 0.0f, false));
    sim.add_army("ARMY_1", "ARMY_1");
    sim.add_army("ARMY_2", "ARMY_2");
    sim.set_playable_rect(10, 10, 100, 100);
    Unit jammer;
    jammer.set_army(1);
    osc::blueprints::UnitFootprints f;
    f.main.size_x = 2;
    f.main.size_z = 1;
    jammer.set_footprints(f);
    // Inset 2 (its larger side) from each edge of (10, 10)-(100, 100)
    CHECK(sim.fake_known_now(jammer, Vector3{11.5f, 0, 50}, 0));
    CHECK_FALSE(sim.fake_known_now(jammer, Vector3{12.0f, 0, 50}, 0));
    CHECK_FALSE(sim.fake_known_now(jammer, Vector3{98.0f, 0, 97.5f}, 0));
    CHECK(sim.fake_known_now(jammer, Vector3{98.5f, 0, 50}, 0));
    CHECK(sim.fake_known_now(jammer, Vector3{50, 0, 9}, 0)); // off it
    // An army that may use the whole map: the map's own edges, inset alike
    sim.get_army(0)->set_use_whole_map(true);
    CHECK_FALSE(sim.fake_known_now(jammer, Vector3{11.5f, 0, 50}, 0));
    CHECK_FALSE(sim.fake_known_now(jammer, Vector3{126.0f, 0, 50}, 0));
    CHECK(sim.fake_known_now(jammer, Vector3{127.0f, 0, 50}, 0));
}
