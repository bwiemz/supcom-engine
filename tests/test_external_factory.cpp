// FAF's external factory, a copy of zxa0002 (RULEUMT_None), builds while attached to its unit.

#include <catch2/catch_test_macros.hpp>

#include "map/heightmap.hpp"
#include "map/terrain.hpp"
#include "sim/manipulator.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"

extern "C" {
#include <lua.h>
}

#include <memory>
#include <vector>

using osc::sim::CommandType;
using osc::sim::SimState;
using osc::sim::Unit;
using osc::sim::UnitCommand;

namespace {

struct LuaGuard {
    lua_State* L = lua_open();
    ~LuaGuard() { lua_close(L); }
};

/// A host unit at x = 64 and, attached to it, a unit given a move order.
struct Scene {
    LuaGuard g;
    SimState sim{g.L, nullptr};
    Unit* host = nullptr;
    Unit* rider = nullptr;
    explicit Scene(bool external_factory) {
        std::vector<osc::u16> heights(129 * 129, 1000);
        osc::map::Heightmap hm(128, 128, 1.0f / 128.0f, std::move(heights));
        sim.set_terrain(std::make_unique<osc::map::Terrain>(std::move(hm), 0.0f, false));
        sim.build_pathfinding_grid();
        sim.add_army("ARMY_1", "ARMY_1");
        sim.set_victory_condition("sandbox");
        host = add(64.0f);
        rider = add(64.0f);
        if (external_factory) {
            rider->add_category("EXTERNALFACTORYUNIT");
            rider->set_motion_type("RULEUMT_None");
        }
        rider->set_parent(host->entity_id(), -1);
        UnitCommand move;
        move.type = CommandType::Move;
        move.target_pos = {80.0f, 8.3f, 64.0f};
        rider->push_command(move, true);
    }
    Unit* add(float x) {
        auto u = std::make_unique<Unit>();
        u->set_army(0);
        u->set_motion_type("RULEUMT_Land");
        u->set_position({x, 8.3f, 64.0f});
        u->set_max_health(100.0f);
        u->set_health(100.0f);
        auto* raw = u.get();
        sim.entity_registry().register_entity(std::move(u));
        return raw;
    }
};

} // namespace

TEST_CASE("An attached unit starts no order; FAF's external factory does", "[orders][attach]") {
    Scene plain(false);
    for (int t = 0; t < 3; ++t) plain.sim.tick();
    REQUIRE(!plain.rider->command_queue().empty());
    CHECK_FALSE(plain.rider->command_queue().front().begun);

    // (begun, and maybe done: an attached unit's move ends where it is)
    Scene factory(true);
    for (int t = 0; t < 3; ++t) factory.sim.tick();
    const auto& queue = factory.rider->command_queue();
    CHECK((queue.empty() || queue.front().begun));
}
