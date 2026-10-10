// --air-attack-run-test: a winged bomber attacks as Moho flies it
// (docs/plans/2026-10-05-air-attack-runs-design.md).
//
// A UEF T1 bomber (UEA0103) is ordered to attack a power generator 100 out:
// 1. it flies at it and releases short of it, where its bombs, leaving with
//    its speed and falling, reach the target;
// 2. its bombs land on the target and hurt it;
// 3. each bomb leaves at the bomber's speed, aimed flat at the target;
// 4. it never stops in the air while it attacks: it overflies, breaks off and
//    comes round, and makes another run.
//
// Before them, a probe of the retail game: a UEF interceptor attacking a
// bomber flying by, and a UEF bomber's run on a power generator, each from
// rest, against retail's track up to its first random draw.
//
// The engine had parked an attacking aircraft at its longest weapon's range
// (the bomb's 40) and hung it there, and the bomb only fell within 3 of the
// target: a bomber given an attack order never dropped.

#include "integration_tests.hpp"

#include "core/test_status.hpp"
#include "lua/lua_state.hpp"
#include "sim/projectile.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

extern "C" {
#include <lua.h>
}

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

namespace osc::test {

namespace {

u32 global_id(TestContext& ctx, const char* name) {
    lua_State* L = ctx.lua_state.raw();
    lua_pushstring(L, name);
    lua_rawget(L, LUA_GLOBALSINDEX);
    const auto id = lua_isnumber(L, -1) ? static_cast<u32>(lua_tonumber(L, -1)) : 0u;
    lua_pop(L, 1);
    return id;
}

f32 flat(const sim::Vector3& a, const sim::Vector3& b) {
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.z - b.z) * (a.z - b.z));
}

} // namespace

void test_air_attack_run(TestContext& ctx) {
    spdlog::info("=== AIR ATTACK RUN TEST: a bomber's runs, as Moho flies them ===");
    int pass = 0, fail = 0;
    const auto record = [&](bool ok, const std::string& what) {
        if (ok) {
            pass++;
            spdlog::info("[PASS] {}", what);
        } else {
            fail++;
            test_status::fail("[FAIL] {}", what);
        }
    };

    {
        auto probe = ctx.lua_state.do_string(R"(
            local function make(bp, army, x, z)
                return CreateUnitHPR(bp, army, x, GetTerrainHeight(x, z), z, 0, 0, 0)
            end
            __run_i = make('uea0102', 'ARMY_1', 260, 740)
            __run_t = make('uea0103', 'ARMY_2', 260, 860)
            __run_b = make('uea0103', 'ARMY_1', 700, 80)
            __run_p = make('ueb1101', 'ARMY_2', 700, 180)
            for _, u in {__run_t, __run_p} do u:SetCanTakeDamage(false) end
            __run_t:SetFireState(1)
            IssueMove({__run_t}, {560, GetTerrainHeight(260, 860), 860})
            IssueAttack({__run_i}, __run_t)
            IssueAttack({__run_b}, __run_p)
            __run_ids = {}
            for i, u in {__run_i, __run_t, __run_b} do __run_ids[i] = tonumber(u:GetEntityId()) end
            __run_i_id, __run_t_id, __run_b_id = __run_ids[1], __run_ids[2], __run_ids[3]
        )");
        record(static_cast<bool>(probe), probe ? "probe setup" : "probe: " + probe.error().message);
        if (!probe) {
            return;
        }
        auto& reg = ctx.sim.entity_registry();
        const auto* fighter = static_cast<const sim::Unit*>(reg.find(global_id(ctx, "__run_i_id")));
        const auto* quarry = static_cast<const sim::Unit*>(reg.find(global_id(ctx, "__run_t_id")));
        const auto* bomber = static_cast<const sim::Unit*>(reg.find(global_id(ctx, "__run_b_id")));
        if (!fighter || !quarry || !bomber) {
            return;
        }
        // A probe of the retail game, its tick k + 1: tick, x, z, heading, bank.
        struct Sample {
            int tick;
            f32 x, z, heading, bank;
        };
        const std::vector<Sample> fighter_ref = {
            {20, 260.00f, 757.29f, 0.001f, 0.0f},    {60, 263.41f, 815.10f, 0.337f, -0.003f},
            {82, 275.55f, 845.13f, 1.003f, -0.320f}, {86, 279.30f, 849.55f, 1.149f, -0.399f},
            {90, 283.63f, 853.31f, 1.141f, -0.261f}, {120, 323.79f, 870.04f, 1.525f, -0.025f},
            {160, 378.65f, 870.92f, 1.664f, 0.004f}, {240, 459.26f, 865.97f, 1.625f, 0.0f},
            {330, 549.13f, 861.12f, 1.625f, 0.0f},
        };
        const std::vector<Sample> bomber_ref = {
            {20, 700.05f, 91.65f, 0.004f, 0.0f},
            {60, 700.25f, 130.37f, 0.006f, 0.0f},
            {90, 700.17f, 160.25f, 6.276f, 0.0f},
            {120, 699.92f, 190.21f, 6.277f, 0.0f},
        };
        const auto turn = [](f32 a, f32 b) {
            f32 d = std::fmod(std::fabs(a - b), 6.2831853f);
            return std::min(d, 6.2831853f - d);
        };
        f32 fighter_off = 0, fighter_turn = 0, bomber_off = 0, bomber_turn = 0;
        int fighter_run = 0, bomber_run = 0;
        f32 trail_lo = 1e9f, trail_hi = 0;
        for (int k = 0; k <= 330; ++k) {
            const auto match = [&](const std::vector<Sample>& ref, const sim::Unit& u, f32& off,
                                   f32& turned) {
                for (const Sample& s : ref) {
                    if (s.tick == k) {
                        off = std::max(off, std::hypot(u.position().x - s.x, u.position().z - s.z));
                        turned = std::max({turned, turn(u.heading(), s.heading),
                                           std::fabs(u.bank_angle() - s.bank)});
                    }
                }
            };
            match(fighter_ref, *fighter, fighter_off, fighter_turn);
            match(bomber_ref, *bomber, bomber_off, bomber_turn);
            if (fighter_run == 0 && fighter->has_unit_state("MakingAttackRun")) {
                fighter_run = k + 1;
            }
            if (bomber_run == 0 && bomber->has_unit_state("MakingAttackRun")) {
                bomber_run = k + 1;
            }
            if (k >= 200) {
                const auto a = fighter->position();
                const auto b = quarry->position();
                const f32 d = std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) +
                                        (a.z - b.z) * (a.z - b.z));
                trail_lo = std::min(trail_lo, d);
                trail_hi = std::max(trail_hi, d);
            }
            ctx.sim.tick();
        }
        record(fighter_off < 1.0f && fighter_turn < 0.05f && fighter_run == 77,
               fmt::format("Probe 1: an interceptor attacking a bomber flying by keeps within 1 of "
                           "retail's track ({:.2f}), its heading and bank within 0.05 ({:.3f}); "
                           "its run starts on retail's tick 77 ({})",
                           fighter_off, fighter_turn, fighter_run));
        record(
            trail_lo > 6.5f && trail_hi < 7.4f,
            fmt::format("Probe 2: it trails the bomber at retail's 6.6 to 7.3 ({:.2f} to {:.2f})",
                        trail_lo, trail_hi));
        record(bomber_off < 0.1f && bomber_turn < 0.01f && bomber_run == 62,
               fmt::format("Probe 3: a bomber's run on a power generator keeps within 0.1 of "
                           "retail's track ({:.3f}), its heading within 0.01 ({:.4f}); its run "
                           "starts on retail's tick 62 ({})",
                           bomber_off, bomber_turn, bomber_run));
        (void)ctx.lua_state.do_string(
            "for _, u in {__run_i, __run_t, __run_b, __run_p} do u:Destroy() end");
    }

    auto r = ctx.lua_state.do_string(R"(
        local x, z = GetArmyBrain('ARMY_1'):GetArmyStartPos()
        local bx, bz = x + 40, z - 60
        __air_bomber = CreateUnitHPR('uea0103', 'ARMY_1', bx, GetTerrainHeight(bx, bz) + 18, bz,
                                     0, 0, 0)
        local tx, tz = bx, bz + 100
        __air_target = CreateUnitHPR('ueb1101', 'ARMY_2', tx, GetTerrainHeight(tx, tz), tz, 0, 0, 0)
        __air_bomber_id = tonumber(__air_bomber:GetEntityId())
        __air_target_id = tonumber(__air_target:GetEntityId())
        IssueAttack({__air_bomber}, __air_target)
    )");
    record(static_cast<bool>(r), r ? "setup" : "setup: " + r.error().message);
    if (!r) return;
    const u32 bomber_id = global_id(ctx, "__air_bomber_id");
    const u32 target_id = global_id(ctx, "__air_target_id");
    auto& registry = ctx.sim.entity_registry();
    const auto* target0 = registry.find(target_id);
    if (!target0) return;
    const sim::Vector3 target_pos = target0->position();
    const f32 target_health = target0->health();

    struct Bomb {
        u32 tick = 0;
        f32 release_dist = 0; // the bomber's from the target, flat
        f32 speed_ratio = 0;  // its speed over the bomber's
        f32 bearing_off = 0;  // its heading's from the target's bearing, radians
        f32 vy = 0;
        sim::Vector3 last;
        bool landed = false;
    };
    std::map<u32, Bomb> bombs;
    std::vector<u32> salvo_ticks;
    f32 min_speed_engaged = 1e9f;
    f32 farthest_past = 0;
    bool engaged = false;
    for (u32 tick = 1; tick <= 900; ++tick) {
        ctx.sim.tick();
        auto* bomber = static_cast<sim::Unit*>(registry.find(bomber_id));
        if (!bomber || bomber->destroyed()) break;
        const f32 to_target = flat(bomber->position(), target_pos);
        if (!engaged && to_target < 50.0f) engaged = true;
        if (engaged && !bomber->command_queue().empty()) {
            const sim::Vector3& v = bomber->velocity();
            min_speed_engaged = std::min(min_speed_engaged, std::sqrt(v.x * v.x + v.z * v.z));
            // How far past the target along its first approach (north).
            farthest_past = std::max(farthest_past, bomber->position().z - target_pos.z);
        }
        registry.for_each([&](sim::Entity& e) {
            if (!e.is_projectile() || e.destroyed()) return;
            auto& p = static_cast<sim::Projectile&>(e);
            if (p.launcher_id != bomber_id) return;
            auto it = bombs.find(p.entity_id());
            if (it == bombs.end()) {
                Bomb b;
                b.tick = tick;
                b.release_dist = to_target;
                const sim::Vector3& bv = bomber->velocity();
                const f32 bs = std::sqrt(bv.x * bv.x + bv.y * bv.y + bv.z * bv.z);
                const f32 ps = std::sqrt(p.velocity.x * p.velocity.x + p.velocity.z * p.velocity.z);
                b.speed_ratio = bs > 0 ? ps / bs : 0;
                const f32 want = std::atan2(target_pos.x - bomber->position().x,
                                            target_pos.z - bomber->position().z);
                f32 off = std::atan2(p.velocity.x, p.velocity.z) - want;
                while (off > 3.14159265f) off -= 6.2831853f;
                while (off < -3.14159265f) off += 6.2831853f;
                b.bearing_off = std::fabs(off);
                b.vy = p.velocity.y;
                it = bombs.emplace(p.entity_id(), b).first;
                if (salvo_ticks.empty() || tick > salvo_ticks.back() + 12)
                    salvo_ticks.push_back(tick);
            }
            it->second.last = p.position();
        });
        for (auto& [id, b] : bombs) {
            const auto* e = registry.find(id);
            if (!b.landed && (!e || e->destroyed())) b.landed = true;
        }
    }

    std::vector<const Bomb*> first;
    for (const auto& [id, b] : bombs)
        if (!salvo_ticks.empty() && b.tick < salvo_ticks.front() + 12) first.push_back(&b);
    int near = 0;
    f32 nearest = 1e9f;
    for (const Bomb* b : first) {
        const f32 d = flat(b->last, target_pos);
        nearest = std::min(nearest, d);
        if (d <= 6.0f) ++near;
    }
    const f32 release = first.empty() ? 0.0f : first.front()->release_dist;
    record(!first.empty() && release >= 15.0f && release <= 40.0f,
           fmt::format("Test 1: it releases short of the target ({} bombs at {:.1f} out, tick {})",
                       first.size(), release, salvo_ticks.empty() ? 0 : salvo_ticks.front()));
    const auto* target = registry.find(target_id);
    const f32 health = target && !target->destroyed() ? target->health() : 0.0f;
    record(near >= 2 && health < target_health,
           fmt::format("Test 2: its bombs land on the target ({} of the first {} within 6, the "
                       "nearest {:.1f}; health {:.0f} of {:.0f})",
                       near, first.size(), nearest, health, target_health));
    // Seen after its first tick, a bomb has fallen for one: at most g/10.
    f32 worst_ratio = 0, worst_off = 0, worst_vy = 0;
    for (const Bomb* b : first) {
        worst_ratio = std::max(worst_ratio, std::fabs(b->speed_ratio - 1.0f));
        worst_off = std::max(worst_off, b->bearing_off);
        worst_vy = std::max(worst_vy, std::fabs(b->vy));
    }
    record(!first.empty() && worst_ratio < 0.15f && worst_off < 0.06f && worst_vy <= 0.5f,
           fmt::format("Test 3: each bomb leaves at the bomber's speed, flat, at the target "
                       "(worst: speed {:.2f} off, bearing {:.3f} rad off, falling {:.2f})",
                       worst_ratio, worst_off, worst_vy));
    record(engaged && min_speed_engaged >= 5.0f && farthest_past > 10.0f && salvo_ticks.size() >= 2,
           fmt::format("Test 4: it never stops while it attacks (slowest {:.1f}), overflies by "
                       "{:.0f} and makes another run ({} salvos in 900 ticks)",
                       min_speed_engaged, farthest_past, salvo_ticks.size()));

    spdlog::info("Air attack run test: {}/{} passed", pass, pass + fail);
}

} // namespace osc::test

namespace osc::test {

// --air-auto-engage-test: an idle aircraft's AutoInitiateAttackCommand
// weapon makes its pick the unit's attack order (Moho's CAcquireTargetTask,
// CheckAutoInitiate):
// 1. an idle fighter (UEA0102) attacks an enemy bomber passing within its
//    reach (25 x TrackingRadius 1.25);
// 2. one on hold fire stays idle;
// 3. one on a move order keeps it;
// 4. an idle bomber (UEA0103) attacks an enemy tank 45 out (its reach 50).
void test_air_auto_engage(TestContext& ctx) {
    spdlog::info("=== AIR AUTO ENGAGE TEST: idle aircraft attack on their own ===");
    int pass = 0, fail = 0;
    const auto record = [&](bool ok, const std::string& what) {
        if (ok) {
            pass++;
            spdlog::info("[PASS] {}", what);
        } else {
            fail++;
            test_status::fail("[FAIL] {}", what);
        }
    };
    auto r = ctx.lua_state.do_string(R"(
        local x, z = GetArmyBrain('ARMY_1'):GetArmyStartPos()
        local function make(bp, army, dx, dz, up)
            local px, pz = x + dx, z + dz
            local u = CreateUnitHPR(bp, army, px, GetTerrainHeight(px, pz) + (up or 0), pz, 0, 0, 0)
            return u, tonumber(u:GetEntityId())
        end
        -- The fighters, 20 west of the bomber's path north.
        __auto_f1, __auto_f1_id = make('uea0102', 'ARMY_1', 20, -40, 20)
        __auto_f2, __auto_f2_id = make('uea0102', 'ARMY_1', 20, 0, 20)
        __auto_f2:SetFireState(1)
        __auto_f3, __auto_f3_id = make('uea0102', 'ARMY_1', 20, 40, 20)
        IssueMove({__auto_f3}, {x + 20, 0, z + 300})
        __auto_bomber, __auto_bomber_id = make('uea0103', 'ARMY_2', 40, -80, 18)
        IssueMove({__auto_bomber}, {x + 40, 0, z + 300})
        -- The bomber and the tank, well away.
        __auto_b1, __auto_b1_id = make('uea0103', 'ARMY_1', 200, -150, 18)
        __auto_tank, __auto_tank_id = make('uel0201', 'ARMY_2', 245, -150, 0)
    )");
    record(static_cast<bool>(r), r ? "setup" : "setup: " + r.error().message);
    if (!r) return;
    lua_State* L = ctx.lua_state.raw();
    const auto id = [&](const char* name) {
        lua_pushstring(L, name);
        lua_rawget(L, LUA_GLOBALSINDEX);
        const auto v = lua_isnumber(L, -1) ? static_cast<u32>(lua_tonumber(L, -1)) : 0u;
        lua_pop(L, 1);
        return v;
    };
    auto& registry = ctx.sim.entity_registry();
    const auto head = [&](u32 unit_id) -> const sim::UnitCommand* {
        auto* e = registry.find(unit_id);
        if (!e || e->destroyed() || !e->is_unit()) return nullptr;
        const auto& q = static_cast<sim::Unit*>(e)->command_queue();
        return q.empty() ? nullptr : &q.front();
    };
    const u32 bomber = id("__auto_bomber_id");
    const u32 tank = id("__auto_tank_id");
    bool f1_attacked = false, b1_attacked = false;
    for (int tick = 0; tick < 120 && !(f1_attacked && b1_attacked); ++tick) {
        ctx.sim.tick();
        if (const auto* c = head(id("__auto_f1_id"));
            c && c->type == sim::CommandType::Attack && c->target_id == bomber)
            f1_attacked = true;
        if (const auto* c = head(id("__auto_b1_id"));
            c && c->type == sim::CommandType::Attack && c->target_id == tank)
            b1_attacked = true;
    }
    record(f1_attacked, "Test 1: an idle fighter attacks an enemy bomber passing by");
    const auto* f2 = head(id("__auto_f2_id"));
    record(!f2 || f2->type != sim::CommandType::Attack,
           "Test 2: a fighter on hold fire stays idle");
    const auto* f3 = head(id("__auto_f3_id"));
    record(f3 && f3->type == sim::CommandType::Move, "Test 3: a fighter on a move order keeps it");
    record(b1_attacked, "Test 4: an idle bomber attacks an enemy tank 45 out");
    spdlog::info("Air auto engage test: {}/{} passed", pass, pass + fail);
}

} // namespace osc::test
