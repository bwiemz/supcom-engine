// --patrol-test: what a patrol takes on along its route, as Moho's
// CUnitPatrolTask does: an engineer reclaims a RECLAIMABLE prop beside it,
// a tank goes for an enemy within its GuardScanRadius.

#include "integration_tests.hpp"
#include "intel_probe.hpp"
#include "render_probe.hpp"

#include "sim/army_brain.hpp"
#include "sim/prop.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"

#include <fmt/format.h>
#include <lua.h>
#include <spdlog/spdlog.h>

#include <string>

namespace osc::test {

namespace {

u32 lua_id(TestContext& ctx, const char* name) {
    lua_pushstring(ctx.L, name);
    lua_rawget(ctx.L, LUA_GLOBALSINDEX);
    const u32 id = lua_isnumber(ctx.L, -1) ? static_cast<u32>(lua_tonumber(ctx.L, -1)) : 0;
    lua_pop(ctx.L, 1);
    return id;
}

const sim::Prop* prop_of(TestContext& ctx, u32 id) {
    const sim::Entity* e = ctx.sim.entity_registry().find(id);
    return e && e->is_prop() ? static_cast<const sim::Prop*>(e) : nullptr;
}

} // namespace

void test_patrol(TestContext& ctx) {
    spdlog::info("=== PATROL TEST: what a patrol takes on along its route ===");
    Tally t;
    const auto spot = quiet_spot(ctx.sim);
    if (!spot) {
        t.check(false, "dry ground 60 from every unit");
        return;
    }
    const f32 sx = spot->x;
    const f32 sz = spot->z;
    const auto y = [&](f32 x, f32 z) { return ctx.sim.terrain()->get_terrain_height(x, z); };
    auto& registry = ctx.sim.entity_registry();
    const auto unit = [&](u32 id) {
        sim::Entity* e = registry.find(id);
        return e && e->is_unit() ? static_cast<sim::Unit*>(e) : nullptr;
    };

    const u32 eng_id = spawn_unit(ctx, "__osc_pt_eng", "uel0105", "ARMY_1", {sx - 20, sz});
    const u32 arty_id = spawn_unit(ctx, "__osc_pt_arty", "uel0304", "ARMY_1", {sx + 40, sz + 65});
    t.check(unit(eng_id) && unit(eng_id)->guard_scan_radius() == 25.0f && unit(arty_id) &&
                unit(arty_id)->guard_scan_radius() == 30.0f,
            "GuardScanRadius: 25 without one in the blueprint, uel0304's 30");

    run_lua(ctx,
            fmt::format("local c = CreateProp({{{0}, {1}, {2}}}, "
                        "'/env/Crystalline/Props/Rocks/CrysCrystal01_prop.bp')\n"
                        "local r = CreateProp({{{3}, {4}, {5}}}, "
                        "'/env/RedRocks/Props/Rock_SM01_prop.bp')\n"
                        "__osc_pt_crystal = tonumber(c:GetEntityId())\n"
                        "__osc_pt_rock = tonumber(r:GetEntityId())\n"
                        "__osc_pt_reclaims = {{}}\n"
                        "local start = __osc_pt_eng.OnStartReclaim\n"
                        "__osc_pt_eng.OnStartReclaim = function(self, target)\n"
                        "    table.insert(__osc_pt_reclaims, tonumber(target:GetEntityId()))\n"
                        "    return start(self, target)\n"
                        "end\n",
                        sx + 10, y(sx + 10, sz + 6), sz + 6, sx - 5, y(sx - 5, sz + 3), sz + 3));
    const u32 crystal = lua_id(ctx, "__osc_pt_crystal");
    const u32 rock = lua_id(ctx, "__osc_pt_rock");
    t.check(prop_of(ctx, crystal) && prop_of(ctx, crystal)->reclaimable_category &&
                prop_of(ctx, rock) && !prop_of(ctx, rock)->reclaimable_category,
            "a prop's RECLAIMABLE from its blueprint (a crystal is, Rock_SM01 is not)");

    auto& economy = ctx.sim.army_at(0)->economy();
    run_lua(ctx, fmt::format("IssuePatrol({{__osc_pt_eng}}, {{{0}, {1}, {2}}})\n"
                             "IssuePatrol({{__osc_pt_eng}}, {{{3}, {4}, {2}}})\n",
                             sx + 40, y(sx + 40, sz), sz, sx - 20, y(sx - 20, sz)));
    for (int i = 0; i < 3000 && prop_of(ctx, crystal); ++i) {
        economy.mass.stored = 0;
        economy.energy.stored = 0;
        ctx.sim.tick();
    }
    run_lua(ctx, fmt::format("__osc_pt_got = ''\n"
                             "for _, id in __osc_pt_reclaims do\n"
                             "    if id == {0} then __osc_pt_got = __osc_pt_got .. 'crystal ' end\n"
                             "    if id == {1} then __osc_pt_got = __osc_pt_got .. 'rock ' end\n"
                             "end\n",
                             crystal, rock));
    lua_pushstring(ctx.L, "__osc_pt_got");
    lua_rawget(ctx.L, LUA_GLOBALSINDEX);
    const std::string got = lua_isstring(ctx.L, -1) ? lua_tostring(ctx.L, -1) : "";
    lua_pop(ctx.L, 1);
    t.check(!prop_of(ctx, crystal) && got == "crystal ",
            "a patrolling engineer reclaims the crystal beside its route, not the rock (" + got +
                ")");

    const u32 tank_id = spawn_unit(ctx, "__osc_pt_tank", "uel0201", "ARMY_1", {sx - 20, sz + 40});
    const u32 enemy_id = spawn_unit(ctx, "__osc_pt_enemy", "uel0201", "ARMY_2", {sx + 10, sz + 55});
    run_lua(ctx, fmt::format("IssuePatrol({{__osc_pt_tank}}, {{{0}, {1}, {2}}})", sx + 40,
                             y(sx + 40, sz + 40), sz + 40));
    bool attacked = false;
    for (int i = 0; i < 300 && !attacked && unit(tank_id); ++i) {
        ctx.sim.tick();
        const auto& q = unit(tank_id)->command_queue();
        attacked = !q.empty() && q.front().from_patrol &&
                   q.front().type == sim::CommandType::Attack && q.front().target_id == enemy_id;
    }
    t.check(attacked, "a patrolling tank breaks off for an enemy 15 off its route");

    spdlog::info("=== PATROL TEST: {} passed, {} failed ===", t.pass, t.fail);
}

} // namespace osc::test
