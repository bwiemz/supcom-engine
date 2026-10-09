// The UI state's units (Moho's UserUnit, M191 step 3): what a UI script
// changes on a unit reaches the sim as a command, so every lockstep peer and
// a replay see it at the same tick. It never changes the live sim.

#include <catch2/catch_test_macros.hpp>

#include "blueprints/blueprint_store.hpp"
#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/user_bindings.hpp"
#include "renderer/input_handler.hpp"
#include "sim/manipulator.hpp"
#include "sim/sim_callback_queue.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/world_snapshot.hpp"
#include "ui/ui_control.hpp"

extern "C" {
#include <lua.h>
}

#include <memory>
#include <string>
#include <unordered_set>
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

TEST_CASE("GetCommandQueue holds the orders given and not yet run", "[userunit]") {
    UiWorld w;
    lua_State* L = w.ui.raw();
    const auto queued = [&] {
        REQUIRE(w.ui.do_string("return table.getn(units[1]:GetCommandQueue())").ok());
        const double n = lua_tonumber(L, -1);
        lua_pop(L, 1);
        return n;
    };
    osc::sim::UnitCommand move;
    move.type = osc::sim::CommandType::Move;
    CHECK(queued() == 0.0);
    w.sim.schedule_command(0, {w.id}, move, true);
    CHECK(queued() == 1.0);

    osc::sim::WorldHistory history;
    history.capture(w.sim);
    osc::lua::set_ui_world_source(L, &history);
    w.sim.schedule_command(0, {w.id}, move, false);
    CHECK(queued() == 2.0);
    CHECK(w.unit().command_queue().empty());
    osc::lua::set_ui_world_source(L, nullptr);
}

TEST_CASE("GetIdleEngineers counts the orders given and not yet run", "[userunit]") {
    UiWorld w;
    osc::ui::UIControlRegistry registry;
    osc::lua::register_ui_bindings(w.ui, registry);
    w.unit().add_category("ENGINEER");
    const auto idle = [&] {
        auto result =
            w.ui.do_string("local e = GetIdleEngineers() return e and table.getn(e) or 0");
        INFO((result.ok() ? std::string() : result.error().message));
        REQUIRE(result.ok());
        const double n = lua_tonumber(w.ui.raw(), -1);
        lua_pop(w.ui.raw(), 1);
        return n;
    };
    CHECK(idle() == 1.0);
    osc::sim::UnitCommand move;
    move.type = osc::sim::CommandType::Move;
    w.sim.schedule_command(0, {w.id}, move, true);
    CHECK(idle() == 0.0);

    w.unit().push_command(move, true);
    osc::sim::UnitCommand stop;
    stop.type = osc::sim::CommandType::Stop;
    w.sim.schedule_command(0, {w.id}, stop, true);
    CHECK(idle() == 1.0);
}

TEST_CASE("A factory's GetCommandQueue is its rally orders, not its builds", "[userunit]") {
    UiWorld w;
    w.unit().add_category("FACTORY");
    osc::sim::UnitCommand build;
    build.type = osc::sim::CommandType::BuildFactory;
    w.unit().push_command(build, false);
    osc::sim::UnitCommand rally;
    rally.type = osc::sim::CommandType::Move;
    rally.target_pos = {40.0f, 0.0f, 60.0f};
    w.unit().add_rally_order(rally);
    auto result = w.ui.do_string(R"(
        local q = units[1]:GetCommandQueue()
        local last = q[table.getn(q)]
        return table.getn(q) .. ' ' .. last.type .. ' ' .. last.position[1] .. ' ' ..
               last.position[3]
    )");
    INFO((result.ok() ? std::string() : result.error().message));
    REQUIRE(result.ok());
    CHECK(std::string(lua_tostring(w.ui.raw(), -1)) == "1 Move 40 60");
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
        -- A string, as Moho's "%d" (faf-re): scripts key tables by it.
        expect('GetEntityId', u:GetEntityId(), ')" +
                                 std::to_string(w.id) + R"(')
        expect('EntityId', u.EntityId, u:GetEntityId())
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
        expect('targetId', queue[1].targetId, ')" +
                                 std::to_string(other_id) + R"(')
        expect('HasUnloadCommandQueuedUp', u:HasUnloadCommandQueuedUp(), true)
        expect('GetFocus', u:GetFocus():GetEntityId(), ')" +
                                 std::to_string(other_id) + R"(')
        expect('GetCreator', u:GetCreator():GetArmy(), 2)
        expect('GetGuardedEntity', u:GetGuardedEntity():GetEntityId(), ')" +
                                 std::to_string(other_id) + R"(')
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

TEST_CASE("A UI script can't issue the engine's own callbacks", "[userunit][simcallback]") {
    // The engine's callbacks (a defeat, a post-load...) start with __osc_;
    // a script's SimCallback naming one is not sent.
    UiWorld w;
    osc::lua::register_user_bindings(w.ui);
    REQUIRE(w.ui.do_string("SimCallback({Func = '__osc_DefeatArmy', Args = {Army = 0}})").ok());
    REQUIRE(w.ui.do_string("SimCallback({Func = '__osc_PostLoad'})").ok());
    CHECK(w.queue.drain().empty());
    REQUIRE(w.ui.do_string("SimCallback({Func = 'OnRename', Args = {}})").ok());
    CHECK(w.queue.drain().size() == 1);
}

TEST_CASE("A guard fighting for its guard still guards, as a UI script sees it", "[userunit]") {
    // Moho keeps the guarded unit while the guard's attack task runs; the
    // engine's fight is an Attack ahead of the Guard (from_guard).
    UiWorld w;
    auto& u = w.unit();
    auto ward = std::make_unique<osc::sim::Unit>();
    ward->set_army(1);
    const osc::u32 ward_id = w.sim.entity_registry().register_entity(std::move(ward));
    osc::sim::UnitCommand fight;
    fight.type = osc::sim::CommandType::Attack;
    fight.from_guard = true;
    u.push_command(fight, false);
    osc::sim::UnitCommand guard;
    guard.type = osc::sim::CommandType::Guard;
    guard.target_id = ward_id;
    u.push_command(guard, false);
    auto result = w.ui.do_string(R"(
        local guarded = units[1]:GetGuardedEntity()
        if not guarded or guarded:GetEntityId() ~= ')" +
                                 std::to_string(ward_id) + R"(' then
            error('guards ' .. tostring(guarded and guarded:GetEntityId()))
        end
    )");
    INFO((result.ok() ? std::string() : result.error().message));
    CHECK(result.ok());
}

TEST_CASE("SelectUnits(nil) and SelectUnits({}) clear the selection", "[userunit][selection]") {
    UiWorld w;
    osc::lua::register_user_bindings(w.ui);
    osc::renderer::InputHandler input;
    lua_State* L = w.ui.raw();
    lua_pushstring(L, "__osc_input_handler");
    lua_pushlightuserdata(L, &input);
    lua_rawset(L, LUA_REGISTRYINDEX);

    for (const char* deselect : {"SelectUnits(nil)", "SelectUnits({})"}) {
        INFO(deselect);
        REQUIRE(w.ui.do_string("SelectUnits(units)").ok());
        REQUIRE(input.selected().size() == 1);
        (void)input.take_selection_event();
        REQUIRE(w.ui.do_string(deselect).ok());
        CHECK(input.selected().empty());
        CHECK(input.take_selection_event());
    }
}

TEST_CASE("A unit aboard can't be selected, unless a POD or a structure", "[selection]") {
    osc::sim::Unit unit;
    unit.set_motion_type("RULEUMT_Land");
    CHECK(osc::renderer::selectable(unit));
    unit.set_transport_id(7);
    CHECK_FALSE(osc::renderer::selectable(unit));
    unit.set_transport_id(0);
    unit.set_parent(7, 0);
    CHECK_FALSE(osc::renderer::selectable(unit));
    unit.add_category("POD");
    CHECK(osc::renderer::selectable(unit));

    osc::sim::Unit structure;
    structure.set_motion_type("RULEUMT_None");
    structure.set_parent(7, 0);
    CHECK(osc::renderer::selectable(structure));
}

TEST_CASE("SelectUnits takes a unit aboard as its transport", "[userunit][selection]") {
    UiWorld w;
    osc::lua::register_user_bindings(w.ui);
    osc::renderer::InputHandler input;
    lua_State* L = w.ui.raw();
    lua_pushstring(L, "__osc_input_handler");
    lua_pushlightuserdata(L, &input);
    lua_rawset(L, LUA_REGISTRYINDEX);
    auto transport = std::make_unique<osc::sim::Unit>();
    transport->set_army(0);
    transport->set_motion_type("RULEUMT_Air");
    const osc::u32 transport_id = w.sim.entity_registry().register_entity(std::move(transport));
    w.unit().set_motion_type("RULEUMT_Land");
    w.unit().set_transport_id(transport_id);

    REQUIRE(w.ui.do_string("SelectUnits(units)").ok());
    CHECK(input.selected() == std::unordered_set<osc::u32>{transport_id});
}

TEST_CASE("A selected unit that boards leaves the selection", "[selection]") {
    UiWorld w;
    osc::renderer::InputHandler input;
    w.unit().set_motion_type("RULEUMT_Land");
    input.set_selected({w.id});
    input.prune_selection(w.sim.entity_registry());
    REQUIRE(input.selected().size() == 1);
    w.unit().set_transport_id(w.id + 1);
    input.prune_selection(w.sim.entity_registry());
    CHECK(input.selected().empty());
}

TEST_CASE("A dying unit leaves the selection and can't be selected or hovered", "[selection]") {
    UiWorld w;
    osc::renderer::InputHandler input;
    w.unit().set_motion_type("RULEUMT_Land");
    input.set_selected({w.id});
    input.prune_selection(w.sim.entity_registry());
    REQUIRE(input.selected().size() == 1);
    REQUIRE(input.unit_under(w.sim, 0.0f, 0.0f) == w.id);
    w.unit().begin_dying();
    input.prune_selection(w.sim.entity_registry());
    CHECK(input.selected().empty());
    CHECK_FALSE(osc::renderer::selectable(w.unit()));
    CHECK(input.unit_under(w.sim, 0.0f, 0.0f) == 0);
}

TEST_CASE("A Ctrl click selects every unit of its blueprint the player has", "[selection]") {
    osc::lua::LuaState lua;
    osc::sim::SimState sim{lua.raw(), nullptr};
    const auto add = [&](const char* bp, osc::i32 army, osc::f32 x, bool being_built = false) {
        auto unit = std::make_unique<osc::sim::Unit>();
        unit->set_army(army);
        unit->set_blueprint_id(bp);
        unit->set_size_xz(1.0f, 1.0f);
        unit->set_size_y(1.0f);
        unit->set_is_being_built(being_built);
        const osc::u32 id = sim.entity_registry().register_entity(std::move(unit));
        sim.entity_registry().find(id)->set_position({x, 0.0f, 10.0f});
        return id;
    };
    const osc::u32 tank = add("uel0201", 0, 10.0f);
    const osc::u32 far_tank = add("uel0201", 0, 900.0f);
    add("uel0201", 0, 500.0f, true);
    add("uel0201", 1, 20.0f);
    const osc::u32 engineer = add("uel0105", 0, 30.0f);
    osc::renderer::InputHandler input;

    input.left_click_at(sim, 10.0f, 10.0f, false, true);
    CHECK(input.selected() == std::unordered_set<osc::u32>{tank, far_tank});

    input.set_selected({engineer});
    input.left_click_at(sim, 200.0f, 10.0f, false, true);
    CHECK(input.selected() == std::unordered_set<osc::u32>{engineer});

    input.left_click_at(sim, 10.0f, 10.0f, true, true);
    CHECK(input.selected() == std::unordered_set<osc::u32>{engineer, tank, far_tank});

    input.set_selected({engineer, tank, far_tank});
    input.left_click_at(sim, 10.0f, 10.0f, true, true);
    CHECK(input.selected() == std::unordered_set<osc::u32>{engineer});
}
