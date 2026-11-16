// --weapon-retarget-test: a weapon whose target dies turns to the next without its script hearing
// of it. Moho's UnitWeapon::SetTarget tells a script OnGotTarget only for a target after none,
// and OnLostTarget only for none after a target (faf-re UnitWeapon.cpp 0x006D5F30, the Steam
// exe's 0x6DC9B0); its CAcquireTargetTask leaves a dead target in the weapon's slot until it next
// looks. Retail's DefaultProjectileWeapon goes to IdleState on OnLostTarget, or packs up
// (WeaponPackingState) a weapon that unpacks; OnGotTarget unpacks or charges it again.
//
// 1. A UEF T3 mobile artillery deployed against one of two power generators in range: the one it
//    shoots at dies. It turns to the other, its weapon's script hearing neither event, never packs
//    up, and fires at it.
// 2. A UEF T1 tank with two power generators in range: the same, its script never going back to
//    IdleState.
// 3. The tank set to hold fire drops its target at its next look (AlwaysRecheckTarget, retail's
//    default), its script hearing OnLostTarget, and takes no other.
//
// Before, the artillery packed up and unpacked again between the two, and the tank's script went
// to IdleState and back.

#include "integration_tests.hpp"
#include "intel_probe.hpp"
#include "render_probe.hpp"
#include "tick_until.hpp"

#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/weapon.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

extern "C" {
#include <lua.h>
}

#include <string>

namespace osc::test {

namespace {

// Each watched weapon's log: the states its script enters, the target events it hears (passed on
// to its state's handler), and its shots.
constexpr const char* kWatch = R"(
    __osc_rt = {}
    local states = {'IdleState', 'RackSalvoChargeState', 'RackSalvoFireReadyState',
                    'RackSalvoFiringState', 'RackSalvoReloadState', 'WeaponUnpackingState',
                    'WeaponPackingState'}
    local change = ChangeState
    rawset(_G, 'ChangeState', function(obj, state)
        local log = __osc_rt[obj]
        if log then
            if type(state) == 'string' then state = obj[state] end
            for _, name in states do
                if obj[name] == state then
                    table.insert(log.states, name)
                    break
                end
            end
        end
        return change(obj, state)
    end)
    local function hear(w, log, event)
        local note
        note = function(self)
            table.insert(log.heard, event)
            rawset(self, event, nil)
            local handler = self[event]
            if handler then handler(self) end
            rawset(self, event, note)
        end
        rawset(w, event, note)
    end
    function __osc_rt_watch(unit)
        local w = unit:GetWeaponByLabel('MainGun')
        local log = {states = {}, heard = {}, shots = 0}
        __osc_rt[w] = log
        hear(w, log, 'OnGotTarget')
        hear(w, log, 'OnLostTarget')
        local fire = w.CreateProjectileAtMuzzle
        w.CreateProjectileAtMuzzle = function(self, muzzle)
            log.shots = log.shots + 1
            return fire(self, muzzle)
        end
        return log
    end
    -- How far `log` has come, to compare with later.
    function __osc_rt_mark(log)
        return {states = table.getn(log.states), heard = table.getn(log.heard), shots = log.shots}
    end
    -- What `log` has added since `mark`: its events and states, space-separated.
    function __osc_rt_since(log, mark, what)
        local list = log[what]
        local out = {}
        for i = mark[what] + 1, table.getn(list) do table.insert(out, list[i]) end
        return table.concat(out, ' ')
    end
)";

std::string lua_text(TestContext& ctx, const std::string& expr) {
    run_lua(ctx, "__osc_rt_text = tostring(" + expr + ")");
    lua_State* L = ctx.L;
    lua_pushstring(L, "__osc_rt_text");
    lua_rawget(L, LUA_GLOBALSINDEX);
    std::string text = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);
    return text;
}

} // namespace

void test_weapon_retarget(TestContext& ctx) {
    spdlog::info("=== WEAPON RETARGET TEST: a weapon turns from a dead target to the next ===");
    // Weapons pick only what their army senses (SimState::weapons_know); this tests the weapon,
    // not intel, so with the fog off it sees everything.
    ctx.sim.set_fog_of_war("none");
    Tally t;
    const auto spot = quiet_spot(ctx.sim);
    if (!spot) {
        t.check(false, "dry ground 60 from every unit");
        return;
    }
    const f32 sx = spot->x;
    const f32 sz = spot->z;
    auto& registry = ctx.sim.entity_registry();
    const auto unit = [&](sim::EntityId id) -> sim::Unit* {
        sim::Entity* e = registry.find(id);
        return e && e->is_unit() && !e->destroyed() ? static_cast<sim::Unit*>(e) : nullptr;
    };
    const auto gun = [&](sim::EntityId id) -> const sim::Weapon* {
        const sim::Unit* u = unit(id);
        if (!u) return nullptr;
        for (const auto& w : u->weapons()) {
            if (w->label == "MainGun") return w.get();
        }
        return nullptr;
    };
    const auto target_of = [&](sim::EntityId id) {
        const sim::Weapon* w = gun(id);
        return w ? w->target_entity_id : sim::EntityId();
    };
    run_lua(ctx, kWatch);

    // 1. The artillery, a generator 40 off and another 50 off.
    const sim::EntityId art_id =
        spawn_unit(ctx, "__osc_rt_art", "uel0304", "ARMY_1", {sx - 20, sz - 30});
    const sim::EntityId gen_a =
        spawn_unit(ctx, "__osc_rt_gen_a", "ueb1101", "ARMY_2", {sx - 20, sz + 10});
    const sim::EntityId gen_b =
        spawn_unit(ctx, "__osc_rt_gen_b", "ueb1101", "ARMY_2", {sx + 30, sz - 30});
    run_lua(ctx, "__osc_rt_gen_a:SetCanTakeDamage(false)\n"
                 "__osc_rt_gen_b:SetCanTakeDamage(false)\n"
                 "__osc_rt_art_log = __osc_rt_watch(__osc_rt_art)");
    if (!tick_until(ctx, "the artillery deploys and fires at a generator", 400, [&] {
            const sim::EntityId at = target_of(art_id);
            return (at == gen_a || at == gen_b) &&
                   lua_true(ctx.lua_state, "__osc_rt_art_log.shots > 0");
        })) {
        return;
    }
    t.check(lua_true(ctx.lua_state, "__osc_rt_art_log.heard[1] == 'OnGotTarget'"),
            "setup: the artillery's script heard OnGotTarget as it took the first");
    const bool a_first = target_of(art_id) == gen_a;
    const sim::EntityId second = a_first ? gen_b : gen_a;
    run_lua(ctx, std::string("__osc_rt_art_kill = __osc_rt_mark(__osc_rt_art_log)\n") +
                     (a_first ? "__osc_rt_gen_a" : "__osc_rt_gen_b") + ":Kill()");
    bool packed = false;
    const auto deployed = [&] {
        const sim::Unit* art = unit(art_id);
        if (!art || !art->immobile()) packed = true;
    };
    const bool turned =
        tick_until(ctx, "Test 1: the artillery turns to the other generator", 100, [&] {
            deployed();
            return target_of(art_id) == second;
        }).has_value();
    if (turned) {
        run_lua(ctx, "__osc_rt_art_turn = __osc_rt_mark(__osc_rt_art_log)");
        tick_until(ctx, "Test 1: the artillery fires at the other generator", 150, [&] {
            deployed();
            return lua_true(ctx.lua_state, "__osc_rt_art_log.shots > __osc_rt_art_turn.shots");
        });
    }
    t.check(turned && lua_true(ctx.lua_state, "__osc_rt_art_log.shots > __osc_rt_art_turn.shots"),
            "Test 1: the artillery turns to the other generator and fires at it");
    t.check(!packed, "Test 1: the artillery stays deployed");
    const std::string art_heard =
        lua_text(ctx, "__osc_rt_since(__osc_rt_art_log, __osc_rt_art_kill, 'heard')");
    t.check(art_heard.empty(), fmt::format("Test 1: its script hears neither OnLostTarget nor "
                                           "OnGotTarget (heard '{}')",
                                           art_heard));
    const std::string art_states =
        lua_text(ctx, "__osc_rt_since(__osc_rt_art_log, __osc_rt_art_kill, 'states')");
    t.check(art_states.find("Packing") == std::string::npos &&
                art_states.find("Unpacking") == std::string::npos,
            fmt::format("Test 1: its script neither packs nor unpacks ({})", art_states));
    run_lua(ctx, std::string("__osc_rt_art:Destroy()\n") +
                     (a_first ? "__osc_rt_gen_b" : "__osc_rt_gen_a") + ":Destroy()");

    // 2. The tank, a generator 10 off and another 15 off.
    const sim::EntityId tank_id =
        spawn_unit(ctx, "__osc_rt_tank", "uel0201", "ARMY_1", {sx + 30, sz + 40});
    const sim::EntityId foe_a =
        spawn_unit(ctx, "__osc_rt_foe_a", "ueb1101", "ARMY_2", {sx + 40, sz + 40});
    const sim::EntityId foe_b =
        spawn_unit(ctx, "__osc_rt_foe_b", "ueb1101", "ARMY_2", {sx + 30, sz + 55});
    run_lua(ctx, "__osc_rt_foe_a:SetCanTakeDamage(false)\n"
                 "__osc_rt_foe_b:SetCanTakeDamage(false)\n"
                 "__osc_rt_tank_log = __osc_rt_watch(__osc_rt_tank)");
    if (!tick_until(ctx, "the tank fires at a generator", 200, [&] {
            const sim::EntityId at = target_of(tank_id);
            return (at == foe_a || at == foe_b) &&
                   lua_true(ctx.lua_state, "__osc_rt_tank_log.shots > 0");
        })) {
        return;
    }
    const bool foe_a_first = target_of(tank_id) == foe_a;
    const sim::EntityId other = foe_a_first ? foe_b : foe_a;
    run_lua(ctx, std::string("__osc_rt_tank_kill = __osc_rt_mark(__osc_rt_tank_log)\n") +
                     (foe_a_first ? "__osc_rt_foe_a" : "__osc_rt_foe_b") + ":Kill()");
    const bool tank_turned =
        tick_until(ctx, "Test 2: the tank turns to the other generator", 60, [&] {
            return target_of(tank_id) == other;
        }).has_value();
    if (tank_turned) {
        run_lua(ctx, "__osc_rt_tank_turn = __osc_rt_mark(__osc_rt_tank_log)");
        tick_until(ctx, "Test 2: the tank fires at the other generator", 40,
                   std::string("__osc_rt_tank_log.shots > __osc_rt_tank_turn.shots"));
    }
    t.check(tank_turned &&
                lua_true(ctx.lua_state, "__osc_rt_tank_log.shots > __osc_rt_tank_turn.shots"),
            "Test 2: the tank turns to the other generator and fires at it");
    const std::string tank_heard =
        lua_text(ctx, "__osc_rt_since(__osc_rt_tank_log, __osc_rt_tank_kill, 'heard')");
    t.check(tank_heard.empty(), fmt::format("Test 2: its script hears neither OnLostTarget nor "
                                            "OnGotTarget (heard '{}')",
                                            tank_heard));
    const std::string tank_states =
        lua_text(ctx, "__osc_rt_since(__osc_rt_tank_log, __osc_rt_tank_kill, 'states')");
    t.check(tank_states.find("IdleState") == std::string::npos,
            fmt::format("Test 2: its script never goes back to IdleState ({})", tank_states));
    if (!tank_turned) return;

    // 3. Hold fire, the other generator still in range.
    run_lua(ctx, "__osc_rt_tank_hold = __osc_rt_mark(__osc_rt_tank_log)\n"
                 "__osc_rt_tank:SetFireState(1)");
    const bool dropped =
        tick_until(ctx, "Test 3: on hold fire, the tank drops its target", 20, [&] {
            const sim::Weapon* w = gun(tank_id);
            return w && !w->has_target() && !w->target_held;
        }).has_value();
    bool retook = false;
    for (int i = 0; i < 30 && dropped; ++i) {
        ctx.sim.tick();
        const sim::Weapon* w = gun(tank_id);
        retook = retook || !w || w->has_target();
    }
    t.check(dropped && !retook, fmt::format("Test 3: on hold fire, the tank drops its target "
                                            "({}) and takes no other ({})",
                                            dropped, retook));
    const std::string hold_heard =
        lua_text(ctx, "__osc_rt_since(__osc_rt_tank_log, __osc_rt_tank_hold, 'heard')");
    const std::string hold_states =
        lua_text(ctx, "__osc_rt_since(__osc_rt_tank_log, __osc_rt_tank_hold, 'states')");
    t.check(hold_heard == "OnLostTarget" && hold_states.find("IdleState") != std::string::npos,
            fmt::format("Test 3: its script hears OnLostTarget ('{}') and goes to IdleState ({})",
                        hold_heard, hold_states));

    spdlog::info("=== WEAPON RETARGET TEST: {} passed, {} failed ===", t.pass, t.fail);
}

} // namespace osc::test
