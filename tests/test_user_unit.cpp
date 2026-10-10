// The UI state's units (Moho's UserUnit, M191 step 3): what a UI script
// changes on a unit reaches the sim as a command, so every lockstep peer and
// a replay see it at the same tick. It never changes the live sim.

#include <catch2/catch_test_macros.hpp>

#include "blueprints/blueprint_store.hpp"
#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/sim_bindings.hpp"
#include "lua/user_bindings.hpp"
#include "map/heightmap.hpp"
#include "renderer/camera.hpp"
#include "renderer/input_handler.hpp"
#include "sim/bone_data.hpp"
#include "sim/manipulator.hpp"
#include "sim/pose.hpp"
#include "sim/sim_callback_queue.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/weapon.hpp"
#include "sim/world_snapshot.hpp"
#include "ui/ui_control.hpp"

extern "C" {
#include <lua.h>
}

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
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

TEST_CASE("The cursor's unit is one its view takes in, but a close OOB-tested one by its ray",
          "[selection]") {
    UiWorld w;
    w.unit().set_motion_type("RULEUMT_Land");
    w.unit().set_position({128.0f, 10.0f, 128.0f});
    const osc::map::Heightmap ground{256, 256, 1.0f / 128.0f,
                                     std::vector<osc::u16>(257 * 257, 10 * 128)};
    osc::renderer::Camera camera;
    camera.set_viewport(1024.0f, 768.0f);
    camera.set_ground(&ground, false, 0.0f);
    camera.init(256.0f, 256.0f);
    camera.set_target(128.0f, 128.0f);
    camera.set_eye_distance(400.0f);
    const auto at = osc::renderer::screen_point(camera.view_proj(1024.0f / 768.0f),
                                                {128.0f, 10.25f, 128.0f}, 1024.0f, 768.0f);
    REQUIRE(at);

    osc::renderer::InputHandler input;
    osc::f32 oob_zoom = 0.0f;
    osc::renderer::CommandModeHooks hooks;
    hooks.pick_blueprint = [&](const std::string&) {
        osc::renderer::PickBlueprint bp;
        bp.oob_test_zoom = oob_zoom;
        return bp;
    };
    input.set_command_mode_hooks(hooks);
    input.set_camera_zoom(400.0f);
    const auto under = [&](osc::f32 dx) {
        osc::f32 wx = 0;
        osc::f32 wz = 0;
        REQUIRE(camera.screen_to_world((*at)[0] + dx, (*at)[1], 1024.0f, 768.0f, 10.0f, wx, wz));
        input.set_cursor_view(camera, 1024.0f, 768.0f, (*at)[0] + dx, (*at)[1], wx, wz);
        return input.unit_under(w.sim, wx, wz);
    };
    CHECK(under(5.0f) == w.id);
    CHECK(under(-5.0f) == w.id);
    CHECK(under(12.0f) == 0);

    oob_zoom = 200.0f;
    CHECK(under(5.0f) == w.id);
    input.set_camera_zoom(150.0f);
    CHECK(under(5.0f) == 0);
    CHECK(under(0.0f) == w.id);
}

TEST_CASE("A unit is picked by its mesh's bounds, scaled by the model and its blueprint",
          "[selection]") {
    UiWorld w;
    w.unit().set_position({128.0f, 10.0f, 128.0f});
    osc::sim::BoneData model;
    model.mesh_bounds = osc::sim::MeshBounds{{-2.5f, 0.0f, -2.5f}, {2.5f, 5.6f, 2.5f}};
    w.unit().set_bone_data(&model);
    osc::renderer::InputHandler input;
    CHECK(input.unit_under(w.sim, 130.3f, 128.0f) == w.id);
    CHECK(input.unit_under(w.sim, 130.7f, 128.0f) == 0);
    model.model_scale = 0.5f;
    CHECK(input.unit_under(w.sim, 129.2f, 128.0f) == w.id);
    CHECK(input.unit_under(w.sim, 130.3f, 128.0f) == 0);

    model.model_scale = 1.0f;
    model.mesh_bounds = osc::sim::MeshBounds{{8.0f, 0.0f, -0.5f}, {15.0f, 2.0f, 0.5f}};
    const osc::map::Heightmap ground{256, 256, 1.0f / 128.0f,
                                     std::vector<osc::u16>(257 * 257, 10 * 128)};
    osc::renderer::Camera camera;
    camera.set_viewport(1024.0f, 768.0f);
    camera.set_ground(&ground, false, 0.0f);
    camera.init(256.0f, 256.0f);
    camera.set_target(128.0f, 128.0f);
    camera.set_eye_distance(100.0f);
    const auto at = osc::renderer::screen_point(camera.view_proj(1024.0f / 768.0f),
                                                {142.0f, 10.5f, 128.0f}, 1024.0f, 768.0f);
    REQUIRE(at);
    osc::f32 scale_x = 1.0f;
    osc::renderer::CommandModeHooks hooks;
    hooks.pick_blueprint = [&](const std::string&) {
        osc::renderer::PickBlueprint bp;
        bp.mesh_scale_x = scale_x;
        return bp;
    };
    input.set_command_mode_hooks(hooks);
    const auto under_cursor = [&] {
        osc::f32 wx = 0;
        osc::f32 wz = 0;
        REQUIRE(camera.screen_to_world((*at)[0], (*at)[1], 1024.0f, 768.0f, 10.0f, wx, wz));
        input.set_cursor_view(camera, 1024.0f, 768.0f, (*at)[0], (*at)[1], wx, wz);
        return input.unit_under(w.sim, wx, wz);
    };
    CHECK(under_cursor() == w.id);
    scale_x = 0.2f;
    CHECK(under_cursor() == 0);
}

TEST_CASE("A drag box takes a unit whose mesh box it meets, scaled by its blueprint without Shift",
          "[selection]") {
    UiWorld w;
    w.unit().set_position({128.0f, 10.0f, 128.0f});
    osc::sim::BoneData model;
    model.mesh_bounds = osc::sim::MeshBounds{{-6.0f, 0.0f, -0.5f}, {6.0f, 2.0f, 0.5f}};
    w.unit().set_bone_data(&model);
    const osc::map::Heightmap ground{256, 256, 1.0f / 128.0f,
                                     std::vector<osc::u16>(257 * 257, 10 * 128)};
    osc::renderer::Camera camera;
    camera.set_viewport(1024.0f, 768.0f);
    camera.set_ground(&ground, false, 0.0f);
    camera.init(256.0f, 256.0f);
    camera.set_target(128.0f, 128.0f);
    camera.set_eye_distance(100.0f);
    const auto vp = camera.view_proj(1024.0f / 768.0f);
    osc::renderer::PickBlueprint bp;
    osc::renderer::CommandModeHooks hooks;
    hooks.pick_blueprint = [&](const std::string&) { return bp; };
    osc::renderer::InputHandler input;
    input.set_player_army(0);
    input.set_command_mode_hooks(hooks);
    const auto boxed = [&](const osc::sim::Vector3& p, bool shift) {
        const auto at = osc::renderer::screen_point(vp, p, 1024.0f, 768.0f);
        REQUIRE(at);
        input.set_selected({});
        input.select_in_box(w.sim, vp, 1024.0f, 768.0f, (*at)[0] - 3, (*at)[1] - 3, (*at)[0] + 3,
                            (*at)[1] + 3, shift);
        return input.selected().count(w.id) == 1;
    };

    CHECK(boxed({133.0f, 11.0f, 128.0f}, false));
    CHECK_FALSE(boxed({137.0f, 11.0f, 128.0f}, false));
    bp.mesh_scale_x = 0.3f;
    CHECK_FALSE(boxed({133.0f, 11.0f, 128.0f}, false));
    CHECK(boxed({133.0f, 11.0f, 128.0f}, true));
    bp = {};
    bp.mesh_scale_z = 0.3f;
    CHECK(boxed({133.0f, 11.0f, 128.0f}, false));

    w.unit().set_orientation(osc::sim::quat_axis_angle('y', 1.5707964f));
    bp = {};
    CHECK(boxed({128.0f, 11.0f, 133.0f}, false));
    bp.mesh_scale_x = 0.3f;
    CHECK_FALSE(boxed({128.0f, 11.0f, 133.0f}, false));

    w.unit().set_orientation(osc::sim::quat_axis_angle('z', 1.5707964f));
    model.mesh_bounds = osc::sim::MeshBounds{{-0.5f, -6.0f, -0.5f}, {0.5f, 6.0f, 0.5f}};
    bp = {};
    CHECK(boxed({133.0f, 10.0f, 128.0f}, false));
    bp.mesh_scale_y = 0.3f;
    CHECK_FALSE(boxed({133.0f, 10.0f, 128.0f}, false));
    CHECK(boxed({133.0f, 10.0f, 128.0f}, true));
}

TEST_CASE("A drag box skips an upgrade's frame without Shift and deselects with Shift",
          "[selection]") {
    UiWorld w;
    w.unit().set_position({128.0f, 10.0f, 128.0f});
    osc::sim::BoneData model;
    model.mesh_bounds = osc::sim::MeshBounds{{-1.0f, 0.0f, -1.0f}, {1.0f, 2.0f, 1.0f}};
    w.unit().set_bone_data(&model);
    auto other_unit = std::make_unique<osc::sim::Unit>();
    other_unit->set_army(0);
    other_unit->set_position({160.0f, 10.0f, 128.0f});
    other_unit->set_bone_data(&model);
    const osc::u32 other = w.sim.entity_registry().register_entity(std::move(other_unit));
    const osc::map::Heightmap ground{256, 256, 1.0f / 128.0f,
                                     std::vector<osc::u16>(257 * 257, 10 * 128)};
    osc::renderer::Camera camera;
    camera.set_viewport(1024.0f, 768.0f);
    camera.set_ground(&ground, false, 0.0f);
    camera.init(256.0f, 256.0f);
    camera.set_target(128.0f, 128.0f);
    camera.set_eye_distance(100.0f);
    const auto vp = camera.view_proj(1024.0f / 768.0f);
    osc::renderer::InputHandler input;
    input.set_player_army(0);
    const auto box = [&](const std::unordered_set<osc::u32>& before, bool shift) {
        const auto at = osc::renderer::screen_point(vp, {128.0f, 11.0f, 128.0f}, 1024.0f, 768.0f);
        REQUIRE(at);
        input.set_selected(before);
        input.select_in_box(w.sim, vp, 1024.0f, 768.0f, (*at)[0] - 3, (*at)[1] - 3, (*at)[0] + 3,
                            (*at)[1] + 3, shift);
        return input.selected();
    };
    using Ids = std::unordered_set<osc::u32>;

    w.unit().set_motion_type("RULEUMT_None");
    w.unit().set_unit_state("BeingUpgraded", true);
    CHECK(box({}, false).empty());
    CHECK(box({}, true) == Ids{w.id});
    w.unit().set_motion_type("RULEUMT_Land");
    CHECK(box({}, false) == Ids{w.id});
    w.unit().set_unit_state("BeingUpgraded", false);
    w.unit().set_motion_type("RULEUMT_None");
    CHECK(box({}, false) == Ids{w.id});

    CHECK(box({w.id, other}, true) == Ids{other});
    CHECK(box({w.id}, true).empty());
    CHECK(box({other}, true) == Ids{w.id, other});
    CHECK(box({w.id, other}, false) == Ids{w.id});
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

TEST_CASE("A selected unit's RequestRefreshUI reports the selection again", "[selection]") {
    UiWorld w;
    osc::renderer::InputHandler input;
    osc::lua::register_moho_bindings(w.sim_lua, w.sim);
    lua_State* S = w.sim_lua.raw();
    lua_newtable(S);
    lua_pushstring(S, "_c_object");
    lua_pushlightuserdata(S, &w.unit());
    lua_rawset(S, -3);
    lua_setglobal(S, "unit");
    input.set_selected({w.id});
    input.prune_selection(w.sim.entity_registry());
    REQUIRE(input.take_selection_event());
    input.prune_selection(w.sim.entity_registry());
    REQUIRE_FALSE(input.take_selection_event());

    REQUIRE(w.sim_lua.do_string("moho.entity_methods.RequestRefreshUI(unit)").ok());
    input.prune_selection(w.sim.entity_registry());
    CHECK(input.take_selection_event());
    CHECK(input.selected() == std::unordered_set<osc::u32>{w.id});
    input.prune_selection(w.sim.entity_registry());
    CHECK_FALSE(input.take_selection_event());
}

TEST_CASE("A selected unit's caps, restrictions and upgrade report the selection again",
          "[selection]") {
    UiWorld w;
    osc::renderer::InputHandler input;
    osc::lua::register_moho_bindings(w.sim_lua, w.sim);
    osc::lua::register_sim_bindings(w.sim_lua, w.sim);
    auto from = std::make_unique<osc::sim::Unit>();
    from->set_army(0);
    const osc::u32 from_id = w.sim.entity_registry().register_entity(std::move(from));
    lua_State* S = w.sim_lua.raw();
    for (const auto& [name, id] : {std::pair{"unit", w.id}, std::pair{"from", from_id}}) {
        lua_newtable(S);
        lua_pushstring(S, "_c_object");
        lua_pushlightuserdata(S, w.sim.entity_registry().find(id));
        lua_rawset(S, -3);
        lua_setglobal(S, name);
    }
    input.set_selected({w.id});
    input.prune_selection(w.sim.entity_registry());
    REQUIRE(input.take_selection_event());

    for (const char* trigger : {
             "M.AddBuildRestriction(unit, 'TECH1')",
             "M.RemoveBuildRestriction(unit, 'TECH1')",
             "M.RestoreBuildRestrictions(unit)",
             "M.AddCommandCap(unit, 'RULEUCC_Move')",
             "M.RemoveCommandCap(unit, 'RULEUCC_Move')",
             "M.RestoreCommandCaps(unit)",
             "M.AddToggleCap(unit, 'RULEUTC_ShieldToggle')",
             "M.RemoveToggleCap(unit, 'RULEUTC_ShieldToggle')",
             "M.RestoreToggleCaps(unit)",
             "NotifyUpgrade(from, unit)",
         }) {
        INFO(trigger);
        input.prune_selection(w.sim.entity_registry());
        REQUIRE_FALSE(input.take_selection_event());
        REQUIRE(w.sim_lua.do_string(std::string("local M = moho.unit_methods ") + trigger).ok());
        input.prune_selection(w.sim.entity_registry());
        CHECK(input.take_selection_event());
    }
}

TEST_CASE("RestoreToggleCaps returns a unit to the toggles it started with", "[selection]") {
    UiWorld w;
    osc::lua::register_moho_bindings(w.sim_lua, w.sim);
    w.unit().add_toggle_cap("RULEUTC_ShieldToggle");
    w.unit().snapshot_toggle_caps();
    lua_State* S = w.sim_lua.raw();
    lua_newtable(S);
    lua_pushstring(S, "_c_object");
    lua_pushlightuserdata(S, &w.unit());
    lua_rawset(S, -3);
    lua_setglobal(S, "unit");
    REQUIRE(w.sim_lua
                .do_string("local M = moho.unit_methods\n"
                           "M.RemoveToggleCap(unit, 'RULEUTC_ShieldToggle')\n"
                           "M.AddToggleCap(unit, 'RULEUTC_CloakToggle')\n"
                           "M.RestoreToggleCaps(unit)")
                .ok());
    CHECK(w.unit().has_toggle_cap("RULEUTC_ShieldToggle"));
    CHECK_FALSE(w.unit().has_toggle_cap("RULEUTC_CloakToggle"));
}

TEST_CASE("A selected unit's settings, Stop and its army's build restrictions report the "
          "selection again",
          "[selection]") {
    osc::lua::LuaState lua;
    osc::blueprints::BlueprintStore store{lua.raw()};
    osc::sim::SimState sim{lua.raw(), &store};
    osc::lua::register_moho_bindings(lua, sim);
    osc::lua::register_sim_bindings(lua, sim);
    sim.add_army("ARMY_1", "ARMY_1");
    sim.add_army("ARMY_2", "ARMY_2");
    lua_State* S = lua.raw();
    std::array<osc::u32, 2> ids{};
    for (const auto& [name, army] : {std::pair{"unit", 0}, std::pair{"enemy", 1}}) {
        auto u = std::make_unique<osc::sim::Unit>();
        u->set_army(army);
        const osc::u32 id = sim.entity_registry().register_entity(std::move(u));
        ids[static_cast<size_t>(army)] = id;
        lua_newtable(S);
        lua_pushstring(S, "_c_object");
        lua_pushlightuserdata(S, sim.entity_registry().find(id));
        lua_rawset(S, -3);
        lua_setglobal(S, name);
    }
    osc::renderer::InputHandler mine;
    osc::renderer::InputHandler theirs;
    mine.set_selected({ids[0]});
    theirs.set_selected({ids[1]});
    for (auto* input : {&mine, &theirs}) {
        input->prune_selection(sim.entity_registry());
        REQUIRE(input->take_selection_event());
    }

    for (const char* trigger : {
             "M.SetPaused(unit, true)",
             "M.SetRepeatQueue(unit, true)",
             "M.SetFireState(unit, 1)",
             "M.SetScriptBit(unit, 'RULEUTC_ShieldToggle', true)",
             "M.ToggleScriptBit(unit, 'RULEUTC_ShieldToggle')",
             "IssueStop({unit})",
             "AddBuildRestriction(1, 'TECH1')",
             "RemoveBuildRestriction(1, 'TECH1')",
         }) {
        INFO(trigger);
        mine.prune_selection(sim.entity_registry());
        REQUIRE_FALSE(mine.take_selection_event());
        REQUIRE(lua.do_string(std::string("local M = moho.unit_methods ") + trigger).ok());
        mine.prune_selection(sim.entity_registry());
        CHECK(mine.take_selection_event());
        theirs.prune_selection(sim.entity_registry());
        CHECK_FALSE(theirs.take_selection_event());
    }
}

TEST_CASE("A selected unit's head order of a refreshing type, taken off, reports the selection "
          "again",
          "[selection]") {
    using osc::sim::CommandType;
    UiWorld w;
    osc::renderer::InputHandler input;
    input.set_selected({w.id});
    input.prune_selection(w.sim.entity_registry());
    REQUIRE(input.take_selection_event());
    osc::u32 next_id = 1;
    for (const auto& [type, refreshes] : {
             std::pair{CommandType::BuildFactory, true},
             std::pair{CommandType::Reclaim, true},
             std::pair{CommandType::Repair, true},
             std::pair{CommandType::Capture, true},
             std::pair{CommandType::TransportLoad, true},
             std::pair{CommandType::TransportUnload, true},
             std::pair{CommandType::WaitForFerry, true},
             std::pair{CommandType::Upgrade, true},
             std::pair{CommandType::Dock, true},
             std::pair{CommandType::Move, false},
             std::pair{CommandType::BuildMobile, false},
             std::pair{CommandType::Attack, false},
         }) {
        INFO(static_cast<int>(type));
        osc::sim::UnitCommand head;
        head.type = type;
        head.command_id = next_id++;
        osc::sim::UnitCommand next;
        next.type = CommandType::Move;
        next.command_id = next_id++;
        w.unit().push_command(head, true);
        w.unit().push_command(next, false);
        w.unit().note_queue_head();
        input.prune_selection(w.sim.entity_registry());
        REQUIRE_FALSE(input.take_selection_event());
        w.unit().remove_command(head.command_id, w.sim.entity_registry(), w.sim_lua.raw());
        w.unit().note_queue_head();
        input.prune_selection(w.sim.entity_registry());
        CHECK(input.take_selection_event() == refreshes);
        w.unit().clear_commands();
        w.unit().note_queue_head();
        input.prune_selection(w.sim.entity_registry());
        input.take_selection_event();
    }
}

TEST_CASE("A repair given and finished in one tick reports the selection again", "[selection]") {
    UiWorld w;
    osc::renderer::InputHandler input;
    input.set_selected({w.id});
    osc::sim::UnitCommand repair;
    repair.type = osc::sim::CommandType::Repair;
    repair.command_id = 1;
    repair.target_id = 9999;
    w.unit().push_command(repair, true);
    input.prune_selection(w.sim.entity_registry());
    REQUIRE(input.take_selection_event());
    w.sim.tick();
    REQUIRE(w.unit().command_queue().empty());
    input.prune_selection(w.sim.entity_registry());
    CHECK(input.take_selection_event());
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

TEST_CASE("UserUnit:CanAttackTarget holds a structure to its weapon's reach", "[userunit]") {
    UiWorld w;
    auto gun = std::make_unique<osc::sim::Weapon>();
    gun->max_range = 10.0f;
    gun->fire_target_layer_caps = osc::sim::parse_layer_caps("Land");
    w.unit().add_weapon(std::move(gun));
    auto target = std::make_unique<osc::sim::Unit>();
    target->set_army(1);
    target->set_position({30.0f, 0.0f, 0.0f});
    const osc::u32 target_id = w.sim.entity_registry().register_entity(std::move(target));
    lua_State* L = w.ui.raw();
    osc::lua::push_units_for_ui(L, {target_id});
    lua_setglobal(L, "far");
    auto result = w.ui.do_string(R"(
        if units[1]:CanAttackTarget(far[1], true) then error('in reach with the range check') end
        if not units[1]:CanAttackTarget(far[1], false) then error('out of reach without it') end
    )");
    INFO((result.ok() ? std::string() : result.error().message));
    CHECK(result.ok());
}
