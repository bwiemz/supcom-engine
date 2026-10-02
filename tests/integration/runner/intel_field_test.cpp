// --intel-field-test (M215g): Moho's intel grids with retail's units.
//
// On dry ground away from the starts: ARMY_2's stealth field generator
// (UEB4203, radar and sonar stealth fields of 24) with an engineer 10 off
// and one 50 off, and 60 away ARMY_1's radar. Retail's SetupIntel switches
// the field on as the generator is finished. ARMY_1's radar finds the far
// engineer, not the near one nor the generator; a sight of the spot shows
// the near one (a field hides from radar, not sight); omni over it beats
// the field; the field off, the radar finds it.
//
// At sea: ARMY_2's submarine (UES0203), dived, and ARMY_1's frigate
// (UES0103: Vision 32, WaterVision 16, radar and sonar), whose SetupIntel
// switches its WaterVision on, it being on the water. 12 off, the frigate
// sees the sub (its WaterVision) and hears it (sonar); 24 off it only hears
// it, though its Vision reaches 32: sight above the water sees nothing
// under it, and radar nothing at all.

#include "integration_tests.hpp"
#include "intel_probe.hpp"
#include "render_probe.hpp"

#include "sim/sim_state.hpp"
#include "sim/unit.hpp"

extern "C" {
#include <lua.h>
}

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <string>

namespace osc::test {

namespace {

std::string senses(u8 recon) {
    std::string s;
    if (recon & sim::SimState::kReconLOS) s += "sight ";
    if (recon & sim::SimState::kReconRadar) s += "radar ";
    if (recon & sim::SimState::kReconSonar) s += "sonar ";
    if (recon & sim::SimState::kReconOmni) s += "omni ";
    return s.empty() ? "nothing" : s.substr(0, s.size() - 1);
}

} // namespace

void test_intel_fields(TestContext& ctx) {
    spdlog::info("=== INTEL FIELD TEST: Moho's intel grids with retail's units (M215g) ===");
    Tally t;
    const auto run = [&](int ticks) {
        for (int i = 0; i < ticks; ++i) ctx.sim.tick();
    };
    const auto lua = [&](const std::string& code) { run_lua(ctx, code); };
    // ARMY_1's recon of a unit now
    const auto recon = [&](u32 id) -> u8 {
        const sim::Entity* e = ctx.sim.entity_registry().find(id);
        return e ? ctx.sim.recon_of(*e, 0) : 0;
    };
    constexpr u8 kLOS = sim::SimState::kReconLOS;
    constexpr u8 kRadar = sim::SimState::kReconRadar;
    constexpr u8 kSonar = sim::SimState::kReconSonar;
    constexpr u8 kOmni = sim::SimState::kReconOmni;

    // --- A stealth field on dry ground
    const auto spot = quiet_spot(ctx.sim);
    if (!spot) {
        t.check(false, "dry ground 60 from every unit");
        return;
    }
    const f32 sx = spot->x;
    const f32 sz = spot->z;
    const u32 generator = spawn_unit(ctx, "__osc_if_gen", "ueb4203", "ARMY_2", {sx, sz});
    const u32 near = spawn_unit(ctx, "__osc_if_near", "uel0105", "ARMY_2", {sx + 10, sz});
    const u32 far = spawn_unit(ctx, "__osc_if_far", "uel0105", "ARMY_2", {sx + 50, sz});
    (void)spawn_unit(ctx, "__osc_if_radar", "ueb3101", "ARMY_1", {sx, sz + 60});
    lua("__osc_if_near:SetImmobile(true) __osc_if_far:SetImmobile(true)\n");
    run(5);
    const auto* gen = static_cast<const sim::Unit*>(ctx.sim.entity_registry().find(generator));
    t.check(gen && gen->is_intel_enabled("RadarStealthField"),
            "retail's SetupIntel switched the generator's field on");
    t.check((recon(far) & kRadar) != 0 && recon(near) == 0 && recon(generator) == 0,
            fmt::format("ARMY_1's radar finds the engineer outside the field ({}), not the one in "
                        "it ({}) nor the generator ({})",
                        senses(recon(far)), senses(recon(near)), senses(recon(generator))));

    lua(fmt::format("CreateVisibleAreaAtPoint(1, {}, 0, {}, 12, 5)\n", sx + 10, sz));
    run(2);
    t.check(recon(near) == kLOS,
            fmt::format("seen, the engineer in the field is in sight, still off radar ({})",
                        senses(recon(near))));

    lua("__osc_if_radar:InitIntel(1, 'Omni', 90) __osc_if_radar:EnableIntel('Omni')\n");
    run(2);
    t.check((recon(near) & (kRadar | kOmni)) == (kRadar | kOmni),
            fmt::format("under omni, the field hides nothing ({})", senses(recon(near))));
    lua("__osc_if_radar:DisableIntel('Omni')\n");
    run(2);
    lua("__osc_if_gen:DisableIntel('RadarStealthField')\n");
    run(2);
    t.check((recon(near) & kRadar) != 0 && (recon(generator) & kRadar) != 0,
            fmt::format("the field off, the radar finds the engineer ({}) and the generator ({})",
                        senses(recon(near)), senses(recon(generator))));

    // --- A submarine and a frigate at sea
    lua(R"(
        local x, z
        for tz = 100, 900, 16 do
            for tx = 60, 500, 16 do
                if not x and GetSurfaceHeight(tx, tz) - GetTerrainHeight(tx, tz) > 8 and
                    GetSurfaceHeight(tx + 24, tz) - GetTerrainHeight(tx + 24, tz) > 8 then
                    x, z = tx, tz
                end
            end
        end
        if not x then error('no deep water') end
        __osc_if_sea = {x, z}
        __osc_if_sub = CreateUnitHPR('ues0203', 'ARMY_2', x, GetSurfaceHeight(x, z), z, 0, 0, 0)
        __osc_if_frigate = CreateUnitHPR('ues0103', 'ARMY_1', x + 12, GetSurfaceHeight(x + 12, z), z,
                                         0, 0, 0)
        __osc_if_frigate:SetImmobile(true)
        IssueDive({__osc_if_sub})
        __osc_if_ids = {tonumber(__osc_if_sub:GetEntityId()), tonumber(__osc_if_frigate:GetEntityId())}
    )");
    lua_State* L = ctx.L;
    lua_pushstring(L, "__osc_if_ids");
    lua_rawget(L, LUA_GLOBALSINDEX);
    u32 sub = 0;
    u32 frigate = 0;
    if (lua_istable(L, -1)) {
        lua_rawgeti(L, -1, 1);
        sub = static_cast<u32>(lua_tonumber(L, -1));
        lua_rawgeti(L, -2, 2);
        frigate = static_cast<u32>(lua_tonumber(L, -1));
        lua_pop(L, 2);
    }
    lua_pop(L, 1);
    const auto layer_of = [&](u32 id) -> std::string {
        const sim::Entity* e = ctx.sim.entity_registry().find(id);
        return e && e->is_unit() ? static_cast<const sim::Unit*>(e)->layer() : "gone";
    };
    for (int i = 0; i < 100 && layer_of(sub) != "Sub"; ++i) run(1);
    run(2);
    const auto* frig = static_cast<const sim::Unit*>(ctx.sim.entity_registry().find(frigate));
    t.check(layer_of(sub) == "Sub" && frig && frig->is_intel_enabled("WaterVision"),
            fmt::format("the sub dived ({}); on the water, the frigate's WaterVision is on",
                        layer_of(sub)));
    t.check(recon(sub) == (kLOS | kSonar),
            fmt::format("12 off, the frigate sees the sub (WaterVision) and hears it ({})",
                        senses(recon(sub))));
    lua("local p = __osc_if_sea\n"
        "Warp(__osc_if_frigate, Vector(p[1] + 24, GetSurfaceHeight(p[1] + 24, p[2]), p[2]))\n");
    run(2);
    t.check(recon(sub) == kSonar,
            fmt::format("24 off, past its WaterVision, only its sonar hears it ({})",
                        senses(recon(sub))));

    spdlog::info("=== INTEL FIELD TEST: {} passed, {} failed ===", t.pass, t.fail);
}

} // namespace osc::test
