// --destroyed-handle-test: a destroyed entity's Lua handle lasts the tick.
// Moho's Destroy() only queues the entity; the deletion queue is drained at
// the end of the beat, so until then scripts still read it (its blueprint,
// id, army), threads forked meanwhile included. Retail's X1CA_002 relies on
// it: CreateArmyGroup(..., wreckage) makes a wrecked base's structures and
// destroys them at once, and the adjacency beams a factory forked for one
// read its blueprint a tick later. After the tick the handle is cut, and
// handles taken from it meanwhile (navigator, weapon, animator) are safe.

#include "integration_tests.hpp"
#include "intel_probe.hpp"
#include "render_probe.hpp"

#include "lua/lua_state.hpp"
#include "sim/sim_state.hpp"

#include <fmt/format.h>
#include <lua.h>
#include <spdlog/spdlog.h>

#include <string>

namespace osc::test {

namespace {

/// The Lua global `name` as a string ("" if it is not one).
std::string global_string(TestContext& ctx, const char* name) {
    lua_pushstring(ctx.L, name);
    lua_rawget(ctx.L, LUA_GLOBALSINDEX);
    std::string s = lua_type(ctx.L, -1) == LUA_TSTRING ? lua_tostring(ctx.L, -1) : "";
    lua_pop(ctx.L, 1);
    return s;
}

/// Whether the Lua global `name` is a handle cut from its C++ object: its
/// _c_object nil or a null pointer.
bool handle_cut(TestContext& ctx, const char* name) {
    lua_pushstring(ctx.L, name);
    lua_rawget(ctx.L, LUA_GLOBALSINDEX);
    bool cut = false;
    if (lua_istable(ctx.L, -1)) {
        lua_pushstring(ctx.L, "_c_object");
        lua_rawget(ctx.L, -2);
        cut = lua_isnil(ctx.L, -1) ||
              (lua_isuserdata(ctx.L, -1) && lua_touserdata(ctx.L, -1) == nullptr);
        lua_pop(ctx.L, 1);
    }
    lua_pop(ctx.L, 1);
    return cut;
}

} // namespace

void test_destroyed_handle(TestContext& ctx) {
    spdlog::info("=== DESTROYED HANDLE TEST: a destroyed entity's handle lasts the tick ===");
    Tally t;
    const auto spot = quiet_spot(ctx.sim);
    if (!spot) {
        t.check(false, "dry ground 60 from every unit");
        return;
    }
    const auto y = [&](f32 x, f32 z) { return ctx.sim.terrain()->get_terrain_height(x, z); };
    const auto lua = [&](const std::string& code) { run_lua(ctx, code); };
    const f32 sx = spot->x;
    const f32 sz = spot->z;

    // Made and destroyed in one chunk, as CreateArmyGroup's wreckage does;
    // what the chunk, and a thread it forks, read of it.
    lua(fmt::format(
        "local u = CreateUnitHPR('ueb1101', 'ARMY_1', {0}, {1}, {2}, 0, 0, 0)\n"
        "local tank = CreateUnitHPR('uel0201', 'ARMY_1', {0}, {1}, {3}, 0, 0, 0)\n"
        "__osc_dh_id = u:GetEntityId()\n"
        "u:Destroy()\n"
        "tank:Destroy()\n"
        "local bp = u:GetBlueprint()\n"
        "__osc_dh_now = tostring(u:BeenDestroyed()) .. ' ' .. tostring(bp and bp.BlueprintId)"
        " .. ' ' .. tostring(bp and bp.SizeX) .. ' ' .. tostring(u:GetEntityId() == __osc_dh_id)"
        " .. ' ' .. tostring(u:GetArmy())\n"
        "__osc_dh_unit, __osc_dh_tank = u, tank\n"
        // Handles taken from it after its Destroy.
        "__osc_dh_nav = tank:GetNavigator()\n"
        "__osc_dh_weapon = tank:GetWeapon(1)\n"
        "__osc_dh_anim = CreateAnimator(tank)\n"
        "ForkThread(function()\n"
        "    local b = __osc_dh_unit:GetBlueprint()\n"
        "    __osc_dh_thread = tostring(b and b.BlueprintId)\n"
        "end)\n",
        sx, y(sx, sz), sz, sz + 10));
    const std::string now = global_string(ctx, "__osc_dh_now");
    t.check(now == "true ueb1101 0.6 true 1",
            "destroyed, it still reads as itself: BeenDestroyed, blueprint, id, army (" + now +
                ")");
    t.check(!handle_cut(ctx, "__osc_dh_nav") && !handle_cut(ctx, "__osc_dh_weapon") &&
                !handle_cut(ctx, "__osc_dh_anim"),
            "meanwhile it still gives a navigator, a weapon and an animator");
    ctx.sim.tick();
    const std::string thread = global_string(ctx, "__osc_dh_thread");
    t.check(thread == "ueb1101",
            "a thread forked meanwhile reads it in the next tick (" + thread + ")");

    // After the tick: cut, and so are the handles taken from it.
    lua("local b = __osc_dh_unit:GetBlueprint()\n"
        "__osc_dh_after = tostring(__osc_dh_unit:BeenDestroyed()) .. ' ' .. tostring(b)\n"
        "__osc_dh_nav:SetGoal({0, 0, 0})\n"
        "__osc_dh_goal = tostring(__osc_dh_nav:GetGoal())\n");
    const std::string after = global_string(ctx, "__osc_dh_after");
    t.check(after == "true nil" && handle_cut(ctx, "__osc_dh_unit"),
            "after the tick its handle is cut (" + after + ")");
    const std::string goal = global_string(ctx, "__osc_dh_goal");
    t.check(goal == "nil", "its navigator does nothing (" + goal + ")");
    t.check(handle_cut(ctx, "__osc_dh_weapon") && handle_cut(ctx, "__osc_dh_anim"),
            "its weapon and animator handles are cut with it");

    // The retail case: a factory and a power generator beside it, the
    // generator made and destroyed at once. The factory's adjacency thread
    // reads the generator's blueprint (a nil one is a thread error, which
    // fails the run); the thread ran if its beams bag holds the generator.
    const f32 fx = sx + 30;
    lua(fmt::format("__osc_dh_fac = CreateUnitHPR('ueb0101', 'ARMY_1', {0}, {1}, {2}, 0, 0, 0)\n"
                    "local pgen = CreateUnitHPR('ueb1101', 'ARMY_1', {3}, {4}, {2}, 0, 0, 0)\n"
                    "__osc_dh_pgen_id = pgen:GetEntityId()\n"
                    "pgen:Destroy()\n",
                    fx, y(fx, sz), sz, fx + 5, y(fx + 5, sz)));
    for (int i = 0; i < 3; ++i) ctx.sim.tick();
    lua("local n = 0\n"
        "for _, v in __osc_dh_fac.AdjacencyBeamsBag or {} do\n"
        "    if v.Unit and v.Unit.EntityId == __osc_dh_pgen_id then n = n + 1 end\n"
        "end\n"
        "__osc_dh_beams = tostring(n)\n");
    const std::string beams = global_string(ctx, "__osc_dh_beams");
    t.check(beams == "1",
            "the factory's adjacency thread ran against the wrecked generator (" + beams + ")");

    spdlog::info("=== DESTROYED HANDLE TEST: {} passed, {} failed ===", t.pass, t.fail);
}

} // namespace osc::test
