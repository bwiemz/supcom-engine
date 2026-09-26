// The UI state's units (Moho's UserUnit, M191 step 3): what a UI script
// changes on a unit reaches the sim as a command, so every lockstep peer and
// a replay see it at the same tick. It never changes the live sim.

#include <catch2/catch_test_macros.hpp>

#include "blueprints/blueprint_store.hpp"
#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "sim/manipulator.hpp"
#include "sim/sim_callback_queue.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/world_snapshot.hpp"

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
    osc::blueprints::BlueprintStore store{ui.raw()};
    osc::sim::SimCallbackQueue queue;
    osc::u32 id = 0;

    UiWorld() {
        auto unit = std::make_unique<osc::sim::Unit>();
        unit->set_army(0);
        unit->set_blueprint_id("uel0001");
        id = sim.entity_registry().register_entity(std::move(unit));
        osc::lua::register_moho_bindings(ui, sim);
        lua_State* L = ui.raw();
        // A UEF commander's blueprint, which the UI reads the unit's
        // categories from.
        ui.set_blueprint_store(&store);
        REQUIRE(ui.do_string("return {BlueprintId = 'uel0001', "
                             "CategoriesHash = {COMMAND = true, UEF = true}}")
                    .ok());
        store.register_blueprint(L, osc::blueprints::BlueprintType::Unit, lua_gettop(L));
        lua_pop(L, 1);
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
    REQUIRE(w.ui.do_string("units[1]:SetCustomName('Fred')").ok());
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
    UiWorld w; // a UEF commander's blueprint
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

TEST_CASE("A UI script sees a unit as the last tick left it", "[userunit]") {
    // UserUnit reads the tick's snapshot, not the live unit: a change the sim
    // makes shows once its tick is over, as Moho's user side sees it.
    UiWorld w;
    w.unit().set_max_health(100);
    w.unit().set_health(80);
    w.sim.tick();
    lua_State* L = w.ui.raw();
    const auto number = [&](const char* code) {
        REQUIRE(w.ui.do_string(code).ok());
        const double v = lua_tonumber(L, -1);
        lua_pop(L, 1);
        return v;
    };
    CHECK(number("return units[1]:GetHealth()") == 80.0);

    // Changed between ticks: the UI still sees the tick's 80.
    w.unit().set_health(30);
    CHECK(number("return units[1]:GetHealth()") == 80.0);
    CHECK(number("return units[1]:GetMaxHealth()") == 100.0);

    // A tick on, it sees 30.
    w.sim.tick();
    CHECK(number("return units[1]:GetHealth()") == 30.0);

    // Gone from the sim, the unit is dead to the UI after its tick.
    w.sim.entity_registry().unregister_entity(w.id);
    CHECK(number("return units[1]:IsDead() and 1 or 0") == 0.0);
    w.sim.tick();
    CHECK(number("return units[1]:IsDead() and 1 or 0") == 1.0);
    CHECK(number("return units[1]:GetHealth()") == 0.0);
}

TEST_CASE("A UI unit has UserUnit's methods, not the sim's", "[userunit]") {
    // The sim's Unit methods change the unit (Kill, SetHealth, Destroy...);
    // the UI's reach it only through commands, as Moho's UserUnit does.
    UiWorld w;
    REQUIRE(w.ui.do_string(R"(
        local u = units[1]
        sim_methods = (u.Kill or u.SetHealth or u.Destroy or u.SetCustomName == nil) and 1 or 0
        user_methods = (u.GetHealth and u.IsIdle and u.ProcessInfo and u.GetSelectionSets) and 1 or 0
    )")
                .ok());
    lua_State* L = w.ui.raw();
    lua_getglobal(L, "sim_methods");
    CHECK(lua_tonumber(L, -1) == 0.0);
    lua_getglobal(L, "user_methods");
    CHECK(lua_tonumber(L, -1) == 1.0);
    lua_pop(L, 2);
}

TEST_CASE("A drawn game's UI reads the renderer's capture of the tick", "[userunit]") {
    // The renderer captures every tick; the UI's unit objects read that
    // capture rather than make their own when it is the sim's current tick.
    UiWorld w;
    w.unit().set_max_health(100);
    w.unit().set_health(80);
    osc::sim::WorldHistory history;
    history.capture(w.sim);
    osc::lua::set_ui_world_source(w.ui.raw(), &history);
    lua_State* L = w.ui.raw();
    const auto health = [&] {
        REQUIRE(w.ui.do_string("return units[1]:GetHealth()").ok());
        const double v = lua_tonumber(L, -1);
        lua_pop(L, 1);
        return v;
    };
    CHECK(health() == 80.0);
    // Captured again at the same tick: the UI reads the new capture, where a
    // copy of its own would still say 80.
    w.unit().set_health(30);
    history.capture(w.sim);
    CHECK(health() == 30.0);
    osc::lua::set_ui_world_source(w.ui.raw(), nullptr);
}

TEST_CASE("Each UserUnit method reads the unit's tick", "[userunit]") {
    UiWorld w;
    auto& u = w.unit();
    u.set_unit_id("uel0001");
    u.set_custom_name("Fred");
    u.set_fuel_ratio(0.5f);
    u.set_shield_ratio(0.25f);
    u.set_build_rate(10);
    u.set_footprint_size(2, 3);
    u.set_auto_mode(true);
    u.set_repeat_queue(true);
    u.set_overcharge_paused(true);
    u.set_auto_surface_mode(true);
    u.set_stunned(2.0);
    u.economy().production_active = true;
    u.economy().production_mass = 2;
    u.economy().production_energy = 20;
    u.economy().consumption_active = true;
    u.economy().consumption_mass = 1;
    u.economy().consumption_energy = 5;
    // Another unit: this one's creator, the focus of its build, and the one
    // it guards.
    auto other = std::make_unique<osc::sim::Unit>();
    other->set_army(1);
    const osc::u32 other_id = w.sim.entity_registry().register_entity(std::move(other));
    u.set_creator_id(other_id);
    u.set_build_target_id(other_id);
    osc::sim::UnitCommand guard;
    guard.type = osc::sim::CommandType::Guard;
    guard.target_id = other_id;
    u.push_command(guard, false);
    osc::sim::UnitCommand unload;
    unload.type = osc::sim::CommandType::TransportUnload;
    u.push_command(unload, false);
    // A unit of no army.
    auto neutral = std::make_unique<osc::sim::Unit>();
    const osc::u32 neutral_id = w.sim.entity_registry().register_entity(std::move(neutral));
    lua_State* L = w.ui.raw();
    osc::lua::push_units_for_ui(L, {neutral_id});
    lua_setglobal(L, "neutral");
    // The UI's first read captures the tick as it stands, before a tick
    // would run the unit's orders.
    auto result = w.ui.do_string(R"(
        local u = units[1]
        local function expect(what, got, want)
            if got ~= want then error(what .. ': ' .. tostring(got) .. ', ' .. tostring(want) .. ' expected') end
        end
        expect('GetEntityId', u:GetEntityId(), )" +
                                 std::to_string(w.id) + R"()
        expect('GetArmy', u:GetArmy(), 1)
        expect('neutral GetArmy', neutral[1]:GetArmy(), -1)
        expect('GetUnitId', u:GetUnitId(), 'uel0001')
        expect('GetBlueprint', u:GetBlueprint().BlueprintId, 'uel0001')
        expect('GetCustomName', u:GetCustomName(), 'Fred')
        expect('GetFuelRatio', u:GetFuelRatio(), 0.5)
        expect('GetShieldRatio', u:GetShieldRatio(), 0.25)
        expect('GetBuildRate', u:GetBuildRate(), 10)
        expect('GetFootPrintSize', u:GetFootPrintSize(), 3)
        expect('IsAutoMode', u:IsAutoMode(), true)
        expect('IsRepeatQueue', u:IsRepeatQueue(), true)
        expect('IsOverchargePaused', u:IsOverchargePaused(), true)
        expect('IsAutoSurfaceMode', u:IsAutoSurfaceMode(), true)
        expect('IsStunned', u:IsStunned(), true)
        expect('IsIdle', u:IsIdle(), false)
        local econ = u:GetEconData()
        expect('massProduced', econ.massProduced, 2)
        expect('energyConsumed', econ.energyConsumed, 5)
        local missiles = u:GetMissileInfo()
        expect('nukeSiloStorageCount', missiles.nukeSiloStorageCount, 0)
        local queue = u:GetCommandQueue()
        expect('GetCommandQueue', table.getn(queue), 2)
        expect('HasUnloadCommandQueuedUp', u:HasUnloadCommandQueuedUp(), true)
        expect('GetFocus', u:GetFocus():GetEntityId(), )" +
                                 std::to_string(other_id) + R"()
        expect('GetCreator', u:GetCreator():GetArmy(), 2)
        expect('GetGuardedEntity', u:GetGuardedEntity():GetEntityId(), )" +
                                 std::to_string(other_id) + R"()
        u:AddSelectionSet('1')
        expect('HasSelectionSet', u:HasSelectionSet('1'), true)
        expect('GetSelectionSets', u:GetSelectionSets()[1], '1')
        u:RemoveSelectionSet('1')
        expect('RemoveSelectionSet', u:HasSelectionSet('1'), false)
        expect('GetStat', u:GetStat('KILLS', 7).Value, 7)
        u:ProcessInfo('SetRepeatQueue', 'false')
    )");
    INFO((result.ok() ? std::string() : result.error().message));
    CHECK(result.ok());
    // ProcessInfo went to the queue, not the unit.
    CHECK(w.unit().repeat_queue());
    auto pending = w.queue.drain();
    REQUIRE_FALSE(pending.empty());
    CHECK(pending.back().func_name == osc::sim::kProcessInfoCallback);
    CHECK(text_arg(pending.back(), "Action") == "SetRepeatQueue");
    CHECK(pending.back().unit_ids == std::vector<osc::u32>{w.id});
}
