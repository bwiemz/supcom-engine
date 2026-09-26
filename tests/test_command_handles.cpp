// A script's Issue* hands back the command its units took, as Moho's returns
// the CUnitCommand's object; IsCommandDone(command) is true once no unit
// holds it. Retail's factories wait on it while a new unit rolls off.

#include <catch2/catch_test_macros.hpp>

#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/sim_bindings.hpp"
#include "sim/manipulator.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"

extern "C" {
#include <lua.h>
}

#include <memory>
#include <string>

using osc::sim::CommandType;
using osc::sim::Unit;
using osc::sim::UnitCommand;

namespace {

struct CommandWorld {
    osc::lua::LuaState lua;
    osc::sim::SimState sim{lua.raw(), nullptr};
    CommandWorld() {
        osc::lua::register_moho_bindings(lua, sim);
        osc::lua::register_sim_bindings(lua, sim);
    }
    osc::u32 spawn() {
        auto u = std::make_unique<Unit>();
        u->set_army(0);
        return sim.entity_registry().register_entity(std::move(u));
    }
    Unit& unit(osc::u32 id) { return *static_cast<Unit*>(sim.entity_registry().find(id)); }
    /// Run `code`; the error message, or "" when it ran.
    std::string run(const std::string& code) {
        auto r = lua.do_string(code);
        return r.ok() ? std::string() : r.error().message;
    }
    bool done(osc::u32 command_id) {
        auto r = lua.do_string("return IsCommandDone(" + std::to_string(command_id) + ")");
        REQUIRE(r.ok());
        lua_State* L = lua.raw();
        const bool v = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        return v;
    }
};

UnitCommand move_to(osc::f32 x, osc::f32 z) {
    UnitCommand c;
    c.type = CommandType::Move;
    c.target_pos = {x, 0.0f, z};
    return c;
}

} // namespace

TEST_CASE("A script's order goes into queues as one command with an id", "[commandhandle]") {
    CommandWorld w;
    const osc::u32 a = w.spawn();
    const osc::u32 b = w.spawn();

    // Both units take the one command; the next order is another.
    const osc::u32 first = w.sim.route_command({a, b}, move_to(100, 0), false);
    REQUIRE(first != 0);
    CHECK(w.unit(a).command_queue().back().command_id == first);
    CHECK(w.unit(b).command_queue().back().command_id == first);
    const osc::u32 second = w.sim.route_command({a}, move_to(0, 100), false);
    CHECK(second != 0);
    CHECK(second != first);

    // An order that comes with its own id (a platoon's) keeps it.
    UnitCommand own = move_to(50, 50);
    own.command_id = 4242;
    CHECK(w.sim.route_command({b}, own, false) == 4242);

    // A Stop queues nothing, and nor does an order no unit takes.
    UnitCommand stop;
    stop.type = CommandType::Stop;
    CHECK(w.sim.route_command({a}, stop, true) == 0);
    CHECK(w.sim.route_command({}, move_to(1, 1), false) == 0);
    CHECK(w.sim.route_command({999999}, move_to(1, 1), false) == 0);

    // A player's order is scheduled, and numbered when it runs.
    w.sim.set_human_input_active(true);
    CHECK(w.sim.route_command({a}, move_to(1, 1), false) == 0);
    w.sim.set_human_input_active(false);
}

TEST_CASE("IsCommandDone: done once no live unit holds the command", "[commandhandle]") {
    CommandWorld w;
    const osc::u32 a = w.spawn();
    const osc::u32 b = w.spawn();
    const osc::u32 cmd = w.sim.route_command({a, b}, move_to(100, 0), false);
    REQUIRE(cmd != 0);
    CHECK_FALSE(w.done(cmd));

    // One unit dropping it isn't enough: the other still has it.
    w.unit(a).clear_commands();
    CHECK(w.sim.command_queued(cmd));
    CHECK_FALSE(w.done(cmd));
    // A dead unit's queue holds nothing.
    w.unit(b).mark_destroyed();
    CHECK_FALSE(w.sim.command_queued(cmd));
    CHECK(w.done(cmd));

    // No such command, or none at all: done.
    CHECK(w.done(0));
    CHECK(w.done(123456));
    CHECK(w.run("assert(IsCommandDone(0/0) == true)").empty()); // NaN is no command
}

TEST_CASE("IsCommandDone takes one command", "[commandhandle]") {
    CommandWorld w;
    CHECK(w.run("IsCommandDone()").find("expected 1 args, but got 0") != std::string::npos);
    CHECK(w.run("IsCommandDone(1, 2)").find("expected 1 args, but got 2") != std::string::npos);
    CHECK(w.run("IsCommandDone('move')").find("Expected a game object") != std::string::npos);
    CHECK(w.run("IsCommandDone(nil)").find("Expected a game object") != std::string::npos);
}
