// FAF's engine functions (FA-Binary-Patches' section/): what FAF's scripts
// get from GetDepositsAroundPoint, GetHighlightCommand, CopyToClipboard, the
// commandType of an order, and the Cursor stats GetMouseTerrainElevation
// reads.

#include <catch2/catch_test_macros.hpp>

#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/sim_bindings.hpp"
#include "lua/user_bindings.hpp"
#include "map/heightmap.hpp"
#include "map/terrain.hpp"
#include "renderer/input_handler.hpp"
#include "renderer/world_shapes.hpp"
#include "sim/bone_data.hpp"
#include "sim/manipulator.hpp"
#include "sim/pose.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"
#include "sim/weapon.hpp"
#include "sim/world_snapshot.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <memory>
#include <string>
#include <utility>
#include <vector>

using osc::sim::CommandType;

namespace {

/// A sim on a 64x64 map under water (ground 7.8125, water 10), with one
/// unit and three deposits, and a UI state bound to it as the game binds it.
struct World {
    osc::lua::LuaState sim_lua;
    osc::sim::SimState sim{sim_lua.raw(), nullptr};
    osc::lua::LuaState ui;
    osc::renderer::InputHandler input;
    osc::sim::WorldSnapshot seen;
    osc::u32 id = 0;

    World() {
        std::vector<osc::u16> heights(65 * 65, 1000);
        osc::map::Heightmap hm(64, 64, 1.0f / 128.0f, std::move(heights));
        sim.set_terrain(std::make_unique<osc::map::Terrain>(std::move(hm), 10.0f, true));
        sim.add_army("ARMY_1", "ARMY_1");
        auto unit = std::make_unique<osc::sim::Unit>();
        unit->set_army(0);
        unit->set_blueprint_id("uel0105");
        unit->set_position({5, 0, 5});
        id = sim.entity_registry().register_entity(std::move(unit));
        // In the map's order: mass, hydrocarbon, mass. A mass point's
        // marker is size 1, a hydrocarbon's 3 (retail's save.lua files).
        sim.add_resource_deposit({10.5f, 0, 20.5f, 1.0f, osc::sim::ResourceDeposit::Mass});
        sim.add_resource_deposit({30.5f, 0, 40.5f, 3.0f, osc::sim::ResourceDeposit::Hydrocarbon});
        sim.add_resource_deposit({12.7f, 0, 20.2f, 1.0f, osc::sim::ResourceDeposit::Mass});
        osc::lua::register_moho_bindings(sim_lua, sim);
        osc::lua::register_sim_bindings(sim_lua, sim);
        osc::lua::register_moho_bindings(ui, sim);
        osc::lua::register_user_bindings(ui);
        lua_State* L = ui.raw();
        lua_pushstring(L, "__osc_input_handler");
        lua_pushlightuserdata(L, &input);
        lua_rawset(L, LUA_REGISTRYINDEX);
        input.set_player_army(0);
        input.set_selected({id});
        osc::lua::push_units_for_ui(L, {id});
        lua_setglobal(L, "units");
    }

    osc::sim::Unit& unit() { return *static_cast<osc::sim::Unit*>(sim.entity_registry().find(id)); }

    /// Give the unit `c` (the queue's next order id), and show the UI the
    /// world as it is now.
    osc::u32 give(osc::sim::UnitCommand c, bool clear = true) {
        static osc::u32 next = 100;
        c.command_id = ++next;
        unit().push_command(c, clear);
        osc::sim::capture_world(sim, seen);
        input.set_frame_view(osc::sim::FrameView(&seen, &seen, 1.0f));
        return c.command_id;
    }
};

std::string run(osc::lua::LuaState& state, const char* code) {
    auto r = state.do_string(code);
    return r ? std::string() : r.error().message;
}

// The checks of GetDepositsAroundPoint, the same in the sim and the UI.
constexpr const char* kDepositChecks = R"(
    local function near(a, b) return math.abs(a - b) < 1e-4 end
    local all = GetDepositsAroundPoint(20, 30, 100, 0)
    if table.getn(all) ~= 3 then error('any type: ' .. table.getn(all)) end
    -- in the map's order, each a whole-cell square about its point
    local m = all[1]
    if m.X1 ~= 10 or m.Z1 ~= 20 or m.X2 ~= 11 or m.Z2 ~= 21 or m.Type ~= 1 then
        error('mass square ' .. m.X1 .. ',' .. m.Z1 .. ',' .. m.X2 .. ',' .. m.Z2)
    end
    local h = all[2]
    if h.X1 ~= 29 or h.Z1 ~= 39 or h.X2 ~= 32 or h.Z2 ~= 42 or h.Type ~= 2 then
        error('hydrocarbon square ' .. h.X1 .. ',' .. h.Z1 .. ',' .. h.X2 .. ',' .. h.Z2)
    end
    -- (12.7, 20.2) truncates to the cell from (12, 19): centre (12.5, 19.5)
    local m2 = all[3]
    if m2.X1 ~= 12 or m2.Z1 ~= 19 or m2.X2 ~= 13 or m2.Z2 ~= 20 then
        error('second mass square ' .. m2.X1 .. ',' .. m2.Z1)
    end
    -- the distance is from the square's centre
    if not near(m.Dist, math.sqrt(9.5 * 9.5 + 9.5 * 9.5)) then error('Dist ' .. m.Dist) end
    local mass = GetDepositsAroundPoint(10.5, 20.5, 3, 1)
    if table.getn(mass) ~= 2 or mass[1].Dist ~= 0 or not near(mass[2].Dist, math.sqrt(5)) then
        error('mass about the first: ' .. table.getn(mass))
    end
    if table.getn(GetDepositsAroundPoint(10.5, 20.5, 2, 1)) ~= 1 then
        error('the radius is not a bound')
    end
    local hydro = GetDepositsAroundPoint(10.5, 20.5, 100, 2)
    if table.getn(hydro) ~= 1 or hydro[1].Type ~= 2 then error('hydrocarbon only') end
    -- no Type is any type
    if table.getn(GetDepositsAroundPoint(20, 30, 100)) ~= 3 then error('no Type') end
    -- a NaN answers nothing (the patch's X + Z + Radius test)
    if GetDepositsAroundPoint(0 / 0, 20, 5, 1) ~= nil then error('NaN') end
    if table.getn(GetDepositsAroundPoint(0, 0, 1, 1)) ~= 0 then error('none near') end
)";

} // namespace

TEST_CASE("An order's commandType is Moho's UNITCOMMAND number", "[faf][orders]") {
    // faf-re SSTICommandIssueData.h, as FAF's UnitQueueDataToCommand lists it
    const std::vector<std::pair<CommandType, int>> plain = {
        {CommandType::Stop, 1},          {CommandType::Move, 2},
        {CommandType::Dive, 3},          {CommandType::SiloBuildTactical, 5},
        {CommandType::SiloBuildNuke, 6}, {CommandType::BuildFactory, 7},
        {CommandType::BuildMobile, 8},   {CommandType::Attack, 10},
        {CommandType::Nuke, 12},         {CommandType::Tactical, 13},
        {CommandType::Teleport, 14},     {CommandType::Guard, 15},
        {CommandType::Patrol, 16},       {CommandType::Ferry, 17},
        {CommandType::Reclaim, 19},      {CommandType::Repair, 20},
        {CommandType::Capture, 21},      {CommandType::TransportLoad, 22},
        {CommandType::WaitForFerry, 22}, {CommandType::TransportUnload, 24},
        {CommandType::Upgrade, 27},      {CommandType::Script, 28},
        {CommandType::Enhance, 28},      {CommandType::Sacrifice, 32},
        {CommandType::Overcharge, 34},   {CommandType::AggressiveMove, 35},
        {CommandType::Dock, 39},
    };
    for (const auto& [type, number] : plain) {
        INFO(static_cast<int>(type));
        CHECK(osc::sim::moho_command_number(type, false) == number);
    }
    // A formation's move, attack and attack-move
    CHECK(osc::sim::moho_command_number(CommandType::Move, true) == 4);
    CHECK(osc::sim::moho_command_number(CommandType::Attack, true) == 11);
    CHECK(osc::sim::moho_command_number(CommandType::AggressiveMove, true) == 36);

    // An order: laid out in a formation, a FormAttack, an unload of chosen
    // cargo (TransportUnloadSpecificUnits)
    osc::sim::UnitCommand order;
    order.type = CommandType::Move;
    CHECK(osc::sim::moho_command_number(order) == 2);
    order.formed = true;
    CHECK(osc::sim::moho_command_number(order) == 4);
    order = {};
    order.type = CommandType::Attack;
    order.form_attack = true;
    CHECK(osc::sim::moho_command_number(order) == 11);
    order = {};
    order.type = CommandType::TransportUnload;
    CHECK(osc::sim::moho_command_number(order) == 24);
    order.unload_ids = {7};
    CHECK(osc::sim::moho_command_number(order) == 25);
}

TEST_CASE("A UI unit's queue numbers its orders as Moho does", "[faf][orders]") {
    World w;
    osc::sim::UnitCommand build;
    build.type = CommandType::BuildMobile;
    build.blueprint_id = "ueb1101";
    build.target_pos = {20, 0, 20};
    w.give(build);
    osc::sim::UnitCommand formed;
    formed.type = CommandType::Move;
    formed.target_pos = {30, 0, 30};
    formed.formed = true;
    w.give(formed, false);
    osc::sim::UnitCommand repair;
    repair.type = CommandType::Repair;
    repair.target_pos = {40, 0, 40};
    w.give(repair, false);
    const std::string error = run(w.ui, R"(
        local q = units[1]:GetCommandQueue()
        local got = {}
        for i, c in q do got[i] = c.commandType end
        if got[1] ~= 8 or got[2] ~= 4 or got[3] ~= 20 then
            error(tostring(got[1]) .. ' ' .. tostring(got[2]) .. ' ' .. tostring(got[3]))
        end
    )");
    INFO(error);
    CHECK(error.empty());
}

TEST_CASE("A sim unit's queue gives each order's place, target and build", "[faf][orders]") {
    // FAF's SimGetCommandQueue patch, which copy-queue.lua re-issues from
    World w;
    auto guarded = std::make_unique<osc::sim::Unit>();
    guarded->set_army(0);
    guarded->set_position({33, 7, 44});
    const osc::u32 guarded_id = w.sim.entity_registry().register_entity(std::move(guarded));
    lua_State* S = w.sim_lua.raw();
    for (const auto& [name, id] : {std::pair{"unit", w.id}, std::pair{"guarded", guarded_id}}) {
        lua_newtable(S);
        lua_pushstring(S, "_c_object");
        lua_pushlightuserdata(S, w.sim.entity_registry().find(id));
        lua_rawset(S, -3);
        lua_pushvalue(S, -1);
        w.sim.entity_registry().find(id)->set_lua_table_ref(luaL_ref(S, LUA_REGISTRYINDEX));
        lua_setglobal(S, name);
    }
    lua_pushstring(S, std::to_string(guarded_id).c_str());
    lua_setglobal(S, "guarded_id");

    osc::sim::UnitCommand build;
    build.type = CommandType::BuildMobile;
    build.blueprint_id = "ueb1101";
    build.target_pos = {20, 3, 24};
    w.give(build);
    osc::sim::UnitCommand guard;
    guard.type = CommandType::Guard;
    guard.target_id = guarded_id;
    guard.target_pos = {1, 1, 1};
    w.give(guard, false);
    osc::sim::UnitCommand patrol;
    patrol.type = CommandType::Patrol;
    patrol.target_pos = {50, 2, 60};
    w.give(patrol, false);
    const std::string error = run(w.sim_lua, R"(
        local q = moho.unit_methods.GetCommandQueue(unit)
        if table.getn(q) ~= 3 then error(table.getn(q) .. ' orders') end
        local b, g, p = q[1], q[2], q[3]
        if b.commandType ~= 8 or b.blueprintId ~= 'ueb1101' or b.x ~= 20 or b.y ~= 3 or b.z ~= 24 then
            error('the build')
        end
        if b.target ~= nil or b.targetId ~= nil then error('a build on a target') end
        -- a target's place is where it is
        if g.commandType ~= 15 or g.target ~= guarded or g.targetId ~= guarded_id then
            error('the guard: ' .. tostring(g.targetId))
        end
        if g.x ~= 33 or g.y ~= 7 or g.z ~= 44 then error('the guard at ' .. g.x .. ',' .. g.z) end
        if p.commandType ~= 16 or p.x ~= 50 or p.z ~= 60 or p.blueprintId ~= nil then
            error('the patrol')
        end
    )");
    INFO(error);
    CHECK(error.empty());
}

TEST_CASE("GetDepositsAroundPoint lists the map's deposits about a point", "[faf][deposits]") {
    World w;
    const std::string in_sim = run(w.sim_lua, kDepositChecks);
    INFO("sim: " << in_sim);
    CHECK(in_sim.empty());
    const std::string in_ui = run(w.ui, kDepositChecks);
    INFO("UI: " << in_ui);
    CHECK(in_ui.empty());
}

TEST_CASE("GetHighlightCommand is the waypoint under the cursor's order", "[faf][orders]") {
    World w;
    CHECK(run(w.ui, "if GetHighlightCommand() ~= nil then error('none hovered') end").empty());

    osc::sim::UnitCommand build;
    build.type = CommandType::BuildMobile;
    build.blueprint_id = "ueb1101";
    build.target_pos = {20, 0, 24};
    const osc::u32 built = w.give(build);
    osc::sim::UnitCommand guard;
    guard.type = CommandType::Guard;
    guard.target_id = w.id; // its own id stands in for a guarded unit's
    guard.target_pos = {5, 0, 5};
    const osc::u32 guarded = w.give(guard, false);

    w.input.set_hovered_command(built);
    lua_State* L = w.ui.raw();
    lua_pushnumber(L, built);
    lua_setglobal(L, "built");
    std::string error = run(w.ui, R"(
        local c = GetHighlightCommand()
        if not c then error('nothing') end
        if c.commandType ~= 8 then error('commandType ' .. tostring(c.commandType)) end
        if c.commandId ~= built then error('commandId ' .. tostring(c.commandId)) end
        if c.x ~= 20 or c.z ~= 24 then error('at ' .. c.x .. ',' .. c.z) end
        if c.blueprintId ~= 'ueb1101' then error('blueprintId ' .. tostring(c.blueprintId)) end
        if c.targetId ~= nil then error('a build has no target') end
    )");
    INFO(error);
    CHECK(error.empty());

    w.input.set_hovered_command(guarded);
    lua_pushstring(L, std::to_string(w.id).c_str());
    lua_setglobal(L, "target");
    error = run(w.ui, R"(
        local c = GetHighlightCommand()
        if not c or c.commandType ~= 15 then error('a Guard') end
        if c.targetId ~= target then error('targetId ' .. tostring(c.targetId)) end
        if c.blueprintId ~= nil then error('a Guard builds nothing') end
    )");
    INFO(error);
    CHECK(error.empty());

    // A command id no shown order has
    w.input.set_hovered_command(guarded + 1000);
    CHECK(run(w.ui, "if GetHighlightCommand() ~= nil then error('stale') end").empty());
}

TEST_CASE("CopyToClipboard takes one argument, and needs a window", "[faf]") {
    World w;
    // No renderer: nothing is copied, and nothing errors.
    CHECK(run(w.ui, "if CopyToClipboard('text') ~= false then error('copied') end").empty());
    CHECK_FALSE(run(w.ui, "CopyToClipboard()").empty());
    CHECK_FALSE(run(w.ui, "CopyToClipboard('a', 'b')").empty());
}

TEST_CASE("UI_DrawLine, UI_DrawCircle and UI_DrawRect draw only from OnRenderWorld",
          "[faf][shapes]") {
    // DrawFunc.cpp: outside a world view's OnRenderWorld they raise an error;
    // inside, each draws its quads (the renderer collects them then)
    World w;
    const std::string outside = run(w.ui, "UI_DrawLine({0, 0, 0}, {10, 0, 0}, 'ff00ff00')");
    CHECK(outside.find("outside of OnRenderWorld") != std::string::npos);
    CHECK_FALSE(run(w.ui, "UI_DrawCircle({0, 0, 0}, 5, 'red')").empty());

    osc::renderer::WorldShapes shapes;
    shapes.begin({});
    lua_State* L = w.ui.raw();
    lua_pushstring(L, osc::renderer::kWorldShapesKey);
    lua_pushlightuserdata(L, &shapes);
    lua_rawset(L, LUA_REGISTRYINDEX);
    CHECK(run(w.ui, "UI_DrawLine({0, 0, 0}, {10, 0, 0}, 'ff00ff00')\n"
                    "UI_DrawCircle({0, 0, 0}, 5, 'Red', 0.5)\n"
                    "UI_DrawRect({0, 0, 0}, 4, '80ffffff')\n")
              .empty());
    CHECK(shapes.quads().size() == 1 + osc::renderer::WorldShapes::kCircleSegments + 4);
    CHECK(shapes.quads()[0].color == 0xFF00FF00u);
    CHECK(shapes.quads()[1].color == 0xFFFF0000u);
    // An unknown colour is an error, and draws nothing
    const std::string bad = run(w.ui, "UI_DrawLine({0, 0, 0}, {1, 0, 0}, 'nocolour')");
    CHECK(bad.find("Unknown color") != std::string::npos);
    CHECK_FALSE(run(w.ui, "UI_DrawRect({0, 0, 0}, 'big', 'red')").empty());
    CHECK(shapes.quads().size() == 1 + osc::renderer::WorldShapes::kCircleSegments + 4);
    lua_pushstring(L, osc::renderer::kWorldShapesKey);
    lua_pushnil(L);
    lua_rawset(L, LUA_REGISTRYINDEX);
}

TEST_CASE("The Cursor stats hold the ground under the cursor", "[faf][stats]") {
    World w;
    // FAF's userInit.lua GetMouseTerrainElevation and cursor/depth.lua read
    // these as Moho fills them.
    constexpr const char* kRead = R"(
        __cursor, __cameras, __cursors = {}, 0, 0
        for _, a in __EngineStats.Children do
            if a.Name == 'Camera' then
                __cameras = __cameras + 1
                for _, b in a.Children do
                    if b.Name == 'Cursor' then
                        __cursors = __cursors + 1
                        for _, c in b.Children do __cursor[c.Name] = c.Value end
                    end
                end
            end
        end
    )";
    // Over the sea: the cursor is on the water at 10; the ground is under it.
    osc::lua::update_cursor_stats(w.ui.raw(), {10.5f, 10.0f, 20.25f});
    osc::lua::update_cursor_stats(w.ui.raw(), {12.25f, 10.0f, 30.75f});
    REQUIRE(run(w.ui, kRead).empty());
    const std::string error = run(w.ui, R"(
        if __cameras ~= 1 or __cursors ~= 1 then error('the tree grew') end
        if __cursor.Elevation ~= 1000 / 128 then error('Elevation ' .. tostring(__cursor.Elevation)) end
        if __cursor.Position ~= 'x=12.25,y=10.00,z=30.75' then error(tostring(__cursor.Position)) end
        if __cursor.OCell ~= 'x=12,z=30' then error(tostring(__cursor.OCell)) end
    )");
    INFO(error);
    CHECK(error.empty());
}
