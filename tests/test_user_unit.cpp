// The UI state's units (Moho's UserUnit, M191 step 3): what a UI script
// changes on a unit reaches the sim as a command, so every lockstep peer and
// a replay see it at the same tick. It never changes the live sim.

#include <catch2/catch_test_macros.hpp>

#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "sim/manipulator.hpp"
#include "sim/sim_callback_queue.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"

extern "C" {
#include <lua.h>
}

#include <memory>
#include <string>
#include <variant>

namespace {

/// A sim with one unit, and a UI Lua state bound to it with its callback
/// queue, as the game sets them up.
struct UiWorld {
    osc::lua::LuaState sim_lua;
    osc::sim::SimState sim{sim_lua.raw(), nullptr};
    osc::lua::LuaState ui;
    osc::sim::SimCallbackQueue queue;
    osc::u32 id = 0;

    UiWorld() {
        auto unit = std::make_unique<osc::sim::Unit>();
        unit->set_army(0);
        id = sim.entity_registry().register_entity(std::move(unit));
        osc::lua::register_moho_bindings(ui, sim);
        lua_State* L = ui.raw();
        lua_pushstring(L, "__osc_sim_callback_queue");
        lua_pushlightuserdata(L, &queue);
        lua_rawset(L, LUA_REGISTRYINDEX);
        // The unit as the UI holds it (GetSelectedUnits and the like).
        osc::lua::push_units_for_ui(L, {id});
        lua_setglobal(L, "units");
    }

    osc::sim::Unit& unit() { return *static_cast<osc::sim::Unit*>(sim.entity_registry().find(id)); }
};

std::string text_arg(const osc::sim::SimCallbackEntry& cb, const char* key) {
    const auto it = cb.args.find(key);
    return it != cb.args.end() && std::holds_alternative<std::string>(it->second)
               ? std::get<std::string>(it->second)
               : std::string();
}

} // namespace

TEST_CASE("A UI script's SetCustomName reaches the sim as a command", "[userunit]") {
    // The rename dialog and gamemain's naming of the commander: Moho sends
    // ProcessInfoPair(id, "CustomName", name).
    UiWorld w;
    REQUIRE(w.ui.do_string("moho.entity_methods.SetCustomName(units[1], 'Fred')").ok());
    CHECK(w.unit().custom_name().empty()); // the live sim is untouched

    auto pending = w.queue.drain();
    REQUIRE(pending.size() == 1);
    CHECK(pending[0].func_name == osc::sim::kProcessInfoCallback);
    CHECK(text_arg(pending[0], "Action") == "CustomName");
    CHECK(text_arg(pending[0], "Value") == "Fred");
    CHECK(pending[0].unit_ids == std::vector<osc::u32>{w.id});

    // Into the command stream, as the game loop submits it; it names the
    // unit at the tick.
    for (auto& cb : pending) w.sim.submit_callback(std::move(cb));
    w.sim.tick();
    CHECK(w.unit().custom_name() == "Fred");
}

TEST_CASE("A sim script's SetCustomName names the unit at once", "[userunit]") {
    // The sim's own scripts (a scenario naming a unit) run in the tick: no
    // command, and no callback queue in their state.
    osc::lua::LuaState sim_lua;
    osc::sim::SimState sim(sim_lua.raw(), nullptr);
    osc::lua::register_moho_bindings(sim_lua, sim);
    auto unit = std::make_unique<osc::sim::Unit>();
    const osc::u32 id = sim.entity_registry().register_entity(std::move(unit));
    lua_State* L = sim_lua.raw();
    osc::lua::push_units_for_ui(L, {id});
    lua_setglobal(L, "units");
    REQUIRE(sim_lua.do_string("moho.entity_methods.SetCustomName(units[1], 'Fred')").ok());
    CHECK(static_cast<osc::sim::Unit*>(sim.entity_registry().find(id))->custom_name() == "Fred");
}

TEST_CASE("A UI script's IsInCategory takes a category's name", "[userunit]") {
    // UserUnit:IsInCategory(category) takes a name: retail's UI passes
    // 'COMMAND' (gamemain's OnFirstUpdate, before it names the commander),
    // 'FACTORY', 'STRUCTURE' and the faction's name (the construction panel).
    UiWorld w;
    w.unit().add_category("COMMAND");
    w.unit().add_category("UEF");
    REQUIRE(w.ui.do_string(R"(
        is_command = units[1]:IsInCategory('COMMAND')
        is_uef = units[1]:IsInCategory('UEF')
        is_factory = units[1]:IsInCategory('FACTORY')
    )")
                .ok());
    lua_State* L = w.ui.raw();
    const auto global = [&](const char* name) {
        lua_pushstring(L, name);
        lua_rawget(L, LUA_GLOBALSINDEX);
        const bool v = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        return v;
    };
    CHECK(global("is_command"));
    CHECK(global("is_uef"));
    CHECK_FALSE(global("is_factory"));
}
