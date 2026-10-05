// --need-unpack-test: a unit whose weapon unpacks to fire (AI.NeedUnpack,
// retail's mobile artillery) packs up and goes when ordered to move, as
// Moho's move task drops an Immobile NeedUnpack unit's desired target and
// its acquire task looks for nothing while the unit moves (faf-re
// CUnitMoveTask.cpp, CAcquireTargetTask.cpp). Retail's
// DefaultProjectileWeapon packs up on OnLostTarget, and, packed, calls
// SetImmobile(false).
//
// A UEF T3 mobile artillery deploys against an enemy power generator 60
// off. Ordered 121 away from it:
// 1. its weapon drops the target as the move begins;
// 2. it packs up and moves off;
// 3. while it moves, it takes no target and never deploys again, though
//    the generator stays within its range for a while;
// 4. it gets there, and stays packed with nothing in range.
// Then ordered to attack the generator, with a second generator in range:
// 5. it never takes the second, and deploys against the one it was sent at.
//
// Before, a deployed artillery piece kept its target and never moved.

#include "integration_tests.hpp"
#include "intel_probe.hpp"
#include "render_probe.hpp"

#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/weapon.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <cmath>
#include <string>

namespace osc::test {

void test_need_unpack(TestContext& ctx) {
    spdlog::info("=== NEED UNPACK TEST: a deployed artillery piece packs up to move ===");
    Tally t;
    const auto spot = quiet_spot(ctx.sim);
    if (!spot) {
        t.check(false, "dry ground 60 from every unit");
        return;
    }
    const f32 sx = spot->x;
    const f32 sz = spot->z;
    auto& registry = ctx.sim.entity_registry();
    const auto unit = [&](u32 id) {
        sim::Entity* e = registry.find(id);
        return e && e->is_unit() ? static_cast<sim::Unit*>(e) : nullptr;
    };
    // The weapon target it holds, 0 for none (-1: a ground target).
    const auto target_of = [&](const sim::Unit& u) -> i64 {
        for (const auto& w : u.weapons()) {
            if (w->target_entity_id != 0) return w->target_entity_id;
            if (w->has_ground_target) return -1;
        }
        return 0;
    };

    const u32 art_id = spawn_unit(ctx, "__osc_nu_art", "uel0304", "ARMY_1", {sx - 20, sz + 10});
    const u32 tank_id = spawn_unit(ctx, "__osc_nu_tank", "uel0201", "ARMY_1", {sx + 40, sz + 60});
    const u32 gen_id = spawn_unit(ctx, "__osc_nu_gen", "ueb1101", "ARMY_2", {sx - 20, sz + 70});
    run_lua(ctx, "__osc_nu_gen:SetCanTakeDamage(false)");
    sim::Unit* art = unit(art_id);
    t.check(art && art->need_unpack() && unit(tank_id) && !unit(tank_id)->need_unpack() &&
                unit(gen_id),
            "AI.NeedUnpack read: uel0304's true, uel0201 without one false");
    if (!art || !unit(gen_id)) return;
    run_lua(ctx, "__osc_nu_tank:Destroy()");

    bool deployed = false;
    for (int i = 0; i < 400 && !deployed; ++i) {
        ctx.sim.tick();
        art = unit(art_id);
        if (!art) break;
        deployed = art->immobile() && target_of(*art) == gen_id;
    }
    t.check(deployed, "the artillery deploys against the generator 60 off");
    if (!deployed) return;

    const sim::Vector3 start = art->position();
    const f32 gx = sx + 40;
    const f32 gz = sz - 35;
    run_lua(ctx,
            fmt::format("IssueMove({{__osc_nu_art}}, {{{0}, GetTerrainHeight({0}, {1}), {1}}})", gx,
                        gz));
    ctx.sim.tick();
    art = unit(art_id);
    t.check(art && target_of(*art) == 0, "Test 1: its weapon drops the target as the move begins");

    int packed_at = -1;
    int arrived_at = -1;
    bool redeployed = false;
    bool targeted = false;
    for (int i = 1; i < 900 && art && arrived_at < 0; ++i) {
        ctx.sim.tick();
        art = unit(art_id);
        if (!art) break;
        const bool moving = !art->command_queue().empty() &&
                            art->command_queue().front().type == sim::CommandType::Move;
        if (packed_at < 0 && !art->immobile()) packed_at = i;
        if (packed_at >= 0 && moving && art->immobile()) redeployed = true;
        if (moving && target_of(*art) != 0) targeted = true;
        if (art->command_queue().empty()) arrived_at = i;
    }
    const f32 moved =
        art ? std::hypot(art->position().x - start.x, art->position().z - start.z) : 0.0f;
    t.check(packed_at >= 0 && packed_at <= 150 && moved > 5.0f,
            fmt::format("Test 2: it packs up (tick {}) and moves off ({:.1f})", packed_at, moved));
    t.check(!targeted && !redeployed,
            fmt::format("Test 3: moving, it takes no target ({}) and never deploys again ({})",
                        targeted, redeployed));
    const f32 off = art ? std::hypot(art->position().x - gx, art->position().z - gz) : 1.0e9f;
    t.check(arrived_at >= 0 && off < 3.0f,
            fmt::format("Test 4: it gets there (tick {}, {:.1f} off)", arrived_at, off));
    if (!art || arrived_at < 0) return;

    bool stayed = true;
    for (int i = 0; i < 100 && art; ++i) {
        ctx.sim.tick();
        art = unit(art_id);
        if (art && (art->immobile() || target_of(*art) != 0)) stayed = false;
    }
    t.check(art && stayed, "Test 4: it stays packed with nothing in range");
    if (!art) return;

    // The generator it is sent at is 121 off; a second, 45 off, is in range.
    const u32 near_id = spawn_unit(ctx, "__osc_nu_near", "ueb1101", "ARMY_2", {sx + 40, sz + 10});
    run_lua(ctx, "__osc_nu_near:SetCanTakeDamage(false)\n"
                 "IssueAttack({__osc_nu_art}, __osc_nu_gen)");
    bool took_near = false;
    bool on_gen = false;
    for (int i = 0; i < 900 && art && !on_gen; ++i) {
        ctx.sim.tick();
        art = unit(art_id);
        if (!art) break;
        if (target_of(*art) == near_id) took_near = true;
        on_gen = art->immobile() && target_of(*art) == gen_id;
    }
    t.check(!took_near && on_gen,
            fmt::format("Test 5: sent at the generator, it never takes the one in range ({}) and "
                        "deploys against its own ({})",
                        took_near, on_gen));

    spdlog::info("=== NEED UNPACK TEST: {} passed, {} failed ===", t.pass, t.fail);
}

} // namespace osc::test
