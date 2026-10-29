// --layer-transition-test: retail's Salem (URS0201, AmphibiousFloating,
// Physics.LayerTransitionDuration 4) crossing the shore, as Moho's
// CUnitMotion::UpdateCurrentLayer and TransitionBetweenLayers move it (faf-re).
//
// Made on dry ground by the water and sent into it, then back:
// 1. it reads its blueprint's 4 s;
// 2. on the tick its layer becomes Water it starts a 39-tick blend onto the
//    water's surface, its place on the map fixed meanwhile;
// 3. sent back, on the tick its layer becomes Land it starts one up the
//    shore, moving on toward the ground it is heading for;
// 4. moving on, it walks.

#include "integration_tests.hpp"
#include "render_probe.hpp"
#include "tick_until.hpp"

#include "lua/lua_state.hpp"
#include "sim/script_class.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"

#include <fmt/format.h>
#include <lua.h>
#include <spdlog/spdlog.h>

#include <cmath>
#include <string>

namespace osc::test {

namespace {

/// Ticks until `u`'s layer is `layer` (at most 400); then how many ticks it
/// stays in the blend, and how far it moves on the map meanwhile.
struct Crossing {
    bool crossed = false;
    int blend_ticks = 0;
    f32 moved = 0;
};

Crossing cross(sim::SimState& sim, sim::EntityRegistry& registry, sim::EntityId id,
               const char* layer) {
    Crossing c;
    const auto unit = [&]() -> sim::Unit* {
        sim::Entity* e = registry.find(id);
        return e && e->is_unit() && !e->destroyed() ? static_cast<sim::Unit*>(e) : nullptr;
    };
    const auto crossed =
        tick_until([&] { sim.tick(); }, fmt::format("the Salem's layer becomes {}", layer), 400,
                   [&] {
                       const sim::Unit* u = unit();
                       return u && u->layer() == layer;
                   });
    c.crossed = crossed.has_value();
    sim::Unit* u = unit();
    if (!c.crossed || !u) return c;
    const sim::Vector3 start = u->position();
    // The tick it began on was its first: count it and the rest
    c.blend_ticks = 1;
    while (u && u->in_layer_transition() && c.blend_ticks < 100) {
        sim.tick();
        ++c.blend_ticks;
        u = unit();
    }
    if (u) {
        const f32 dx = u->position().x - start.x;
        const f32 dz = u->position().z - start.z;
        c.moved = std::sqrt(dx * dx + dz * dz);
    }
    return c;
}

} // namespace

void test_layer_transition(TestContext& ctx) {
    spdlog::info("=== LAYER TRANSITION TEST: the Salem crossing the shore ===");
    Tally t;
    // Dry ground within 20 of water 2.5 deep, either way along a row of the
    // map (SCMP_009's shore, where the drown test finds its depths)
    const auto spawn = ctx.lua_state.do_string(
        "local function depth(x, z) return GetSurfaceHeight(x, z) - GetTerrainHeight(x, z) end\n"
        "__osc_lt = false\n"
        "for z = 280, 420, 4 do\n"
        "    local x = 500\n"
        "    while x <= 800 and not __osc_lt do\n"
        "        local dry = depth(x, z) <= 0\n"
        "        for _, dir in {1, -1} do\n"
        "            if not __osc_lt and dry and depth(x - 8 * dir, z) <= 0 and\n"
        "               depth(x + 20 * dir, z) >= 2.5 then\n"
        "                local u = CreateUnitHPR('urs0201', 'ARMY_1', x, GetTerrainHeight(x, z), "
        "z,\n"
        "                                        0, 0, 0)\n"
        "                __osc_lt = {tonumber(u:GetEntityId()), x, z, dir}\n"
        "            end\n"
        "        end\n"
        "        x = x + 0.5\n"
        "    end\n"
        "    if __osc_lt then break end\n"
        "end\n"
        "if not __osc_lt then error('no shore found') end\n");
    t.check(static_cast<bool>(spawn),
            spawn ? std::string("a Salem made on dry ground by the water")
                  : fmt::format("the Salem made: {}", spawn.error().message));
    if (!spawn) return;
    lua_State* L = ctx.L;
    lua_pushstring(L, "__osc_lt");
    lua_rawget(L, LUA_GLOBALSINDEX);
    lua_rawgeti(L, -1, 1);
    const sim::EntityId id = osc::sim::to_entity_id(L, -1, ctx.sim.entity_registry());
    lua_pop(L, 1);
    lua_rawgeti(L, -1, 2);
    const auto x = static_cast<f32>(lua_tonumber(L, -1));
    lua_pop(L, 1);
    lua_rawgeti(L, -1, 3);
    const auto z = static_cast<f32>(lua_tonumber(L, -1));
    lua_pop(L, 1);
    lua_rawgeti(L, -1, 4);
    const auto dir = static_cast<f32>(lua_tonumber(L, -1)); // toward the water
    lua_pop(L, 2);
    auto& registry = ctx.sim.entity_registry();
    auto* salem = static_cast<sim::Unit*>(registry.find(id));
    if (!salem) return;

    // 1.
    t.check(salem->layer() == "Land" && salem->layer_transition_duration() == 4.0f,
            fmt::format("1: on Land ({}), with its blueprint's 4 s ({})", salem->layer(),
                        salem->layer_transition_duration()));

    // 2. Into the water
    sim::UnitCommand into;
    into.type = sim::CommandType::Move;
    into.target_pos = {x + 50.0f * dir, 0.0f, z};
    ctx.sim.route_command({id}, into, true);
    const Crossing in = cross(ctx.sim, registry, id, "Water");
    t.check(in.crossed && in.blend_ticks == 39 && in.moved < 0.01f,
            fmt::format("2: onto the water in 39 ticks ({}), not moving on meanwhile ({:.3f})",
                        in.blend_ticks, in.moved));

    // 3. Back onto the land
    for (int k = 0; k < 120; ++k) ctx.sim.tick(); // out on the water first
    sim::UnitCommand back;
    back.type = sim::CommandType::Move;
    back.target_pos = {x - 20.0f * dir, 0.0f, z};
    ctx.sim.route_command({id}, back, true);
    const Crossing out = cross(ctx.sim, registry, id, "Land");
    t.check(out.crossed && out.blend_ticks == 39 && out.moved > 0.5f,
            fmt::format("3: up the shore in 39 ticks ({}), moving on toward the land ({:.2f})",
                        out.blend_ticks, out.moved));

    // 4. Its script starts AnimationWalk only on a start from Stopped
    for (int k = 0; k < 10; ++k) {
        ctx.sim.tick();
    }
    const auto walks = ctx.lua_state.do_string(
        fmt::format("local u = GetEntityById('{}')\n"
                    "__osc_lt_walk = u and string.format('%s %s %s', tostring(u.Walking),"
                    " tostring(u.SwitchAnims), tostring(u.IsWaiting)) or 'gone'",
                    sim::EntityRegistry::moho_id(id)));
    lua_pushstring(L, "__osc_lt_walk");
    lua_rawget(L, LUA_GLOBALSINDEX);
    const std::string walk =
        walks ? (lua_isstring(L, -1) ? lua_tostring(L, -1) : "?") : walks.error().message;
    lua_pop(L, 1);
    t.check(walk == "true false false",
            fmt::format("4: on the land it plays its walk (Walking, SwitchAnims, IsWaiting: {})",
                        walk));
    spdlog::info("Layer transition test: {} passed, {} failed", t.pass, t.fail);
}

} // namespace osc::test
