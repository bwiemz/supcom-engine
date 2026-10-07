// --footfall-test: Moho's collision detector (CCollisionManipulator, read
// from the binary at 0x637C90) with retail's units. A walker's script
// enables its detector as it moves; each foot bone that drops below 0.1 in
// the unit's frame is a footfall (OnAnimCollision, bone and its x, y, z in
// that frame), and the Galactic Colossus's blueprint makes each one deal
// damage. With a terrain check a bone is tested against the surface
// instead, on the way down and back up -- as a crashing CZAR's and
// Ahwassa's bones are, each once, its script's crash damage dealt there.

#include "integration_tests.hpp"
#include "render_probe.hpp" // Tally

#include "lua/lua_state.hpp"
#include "sim/sim_state.hpp"

#include <spdlog/spdlog.h>

namespace osc::test {

void test_footfall(TestContext& ctx) {
    spdlog::info("=== FOOTFALL TEST: collision detectors on walkers ===");
    Tally t;
    const auto lua = [&](const char* what, const char* code) {
        auto r = ctx.lua_state.do_string(code);
        t.check(static_cast<bool>(r),
                r ? std::string(what) : std::string(what) + ": " + r.error().message);
    };
    const auto run = [&](int ticks) {
        for (int i = 0; i < ticks; ++i) ctx.sim.tick();
    };

    lua("setup: a Galactic Colossus on open, level ground", R"(
        -- Dry, gently rolling land 160 across.
        local x, z
        for tz = 150, 850, 25 do
            for tx = 150, 850, 25 do
                if not x then
                    local h = GetTerrainHeight(tx, tz)
                    local ok = GetSurfaceHeight(tx, tz) <= h
                    for dx = -80, 80, 20 do
                        for dz = -80, 80, 20 do
                            local g = GetTerrainHeight(tx + dx, tz + dz)
                            if math.abs(g - h) > 8 or GetSurfaceHeight(tx + dx, tz + dz) > g then ok = false end
                        end
                    end
                    if ok then x, z = tx, tz end
                end
            end
        end
        if not x then error('no level ground') end
        __osc_ground = {x, z}
        __osc_gc = CreateUnitHPR('ual0401', 'ARMY_1', x, GetTerrainHeight(x, z - 60), z - 60, 0, 0, 0)
        __osc_falls = {}
        local fall = __osc_gc.OnAnimCollision
        __osc_gc.OnAnimCollision = function(self, bone, bx, by, bz)
            table.insert(__osc_falls, {bone = bone, x = bx, y = by, z = bz, tick = GetGameTick()})
            return fall(self, bone, bx, by, bz)
        end
        __osc_footfall_damage = 0
        local damage_area = DamageArea
        DamageArea = function(instigator, pos, radius, amount, kind, friendly, selfdamage)
            if kind == 'ExperimentalFootfall' then __osc_footfall_damage = __osc_footfall_damage + 1 end
            return damage_area(instigator, pos, radius, amount, kind, friendly, selfdamage)
        end
    )");
    run(30);
    lua("Test 1: standing, it has no footfalls (its detector waits to be enabled)", R"(
        if table.getn(__osc_falls) ~= 0 then error(table.getn(__osc_falls) .. ' footfalls standing still') end
    )");
    lua("setup: it walks", R"(
        local x, z = __osc_ground[1], __osc_ground[2]
        IssueMove({__osc_gc}, {x, GetTerrainHeight(x, z + 60), z + 60})
    )");
    run(150);
    lua("Test 2: walking, the feet fall in turn, the left at positive x, the right negative", R"(
        local n = table.getn(__osc_falls)
        if n < 4 then error(n .. ' footfalls in 15 s of walking') end
        local seq = ''
        for _, f in __osc_falls do seq = seq .. string.sub(f.bone, 1, 1) .. string.format('(%.2f,%.2f,%.2f) ', f.x, f.y, f.z) end
        LOG('Footfall test: sequence ' .. seq)
        local left, right = 0, 0
        local left_x, right_x
        for _, f in __osc_falls do
            if f.y >= 0.1 then error(f.bone .. ' fell at ' .. f.y .. ' in the unit frame, not below 0.1') end
            if f.bone == 'Left_Footfall' then
                left = left + 1
                if not left_x then left_x = f.x end
            elseif f.bone == 'Right_Footfall' then
                right = right + 1
                if not right_x then right_x = f.x end
            else
                error('a footfall of ' .. tostring(f.bone))
            end
        end
        if left == 0 or right == 0 then error(left .. ' left and ' .. right .. ' right footfalls') end
        -- Retail's script plays FootFallLeft for x > 0, FootFallRight below
        if not (left_x > 0 and right_x < 0) then
            error('the feet fell on the wrong sides: left x ' .. left_x .. ', right x ' .. right_x)
        end
        -- Under way, they take turns (the walk's start may set a foot down twice)
        for i = 5, n do
            if __osc_falls[i].bone == __osc_falls[i - 1].bone then
                error('the ' .. __osc_falls[i].bone .. ' fell twice running')
            end
        end
        LOG('Footfall test: ' .. n .. ' footfalls; left x ' .. left_x .. ', right x ' .. right_x)
    )");
    lua("Test 3: each footfall deals the blueprint's footfall damage (retail's script)", R"(
        if __osc_footfall_damage < table.getn(__osc_falls) then
            error(__osc_footfall_damage .. ' footfall damage calls for ' .. table.getn(__osc_falls) .. ' footfalls')
        end
    )");
    run(400); // it arrives and stops
    lua("setup: it has stopped", R"(
        if __osc_gc:IsUnitState('Moving') then error('still moving') end
        __osc_falls_stopped = table.getn(__osc_falls)
    )");
    run(60);
    lua("Test 4: stopped, its detector is disabled and no foot falls", R"(
        if table.getn(__osc_falls) ~= __osc_falls_stopped then
            error((table.getn(__osc_falls) - __osc_falls_stopped) .. ' footfalls after it stopped')
        end
    )");

    lua("setup: a tank's root watched against the surface, and one never enabled", R"(
        local x, z = __osc_ground[1] + 40, __osc_ground[2]
        local function watched(enable)
            local tank = CreateUnitHPR('uel0201', 'ARMY_1', x, GetTerrainHeight(x, z), z, 0, 0, 0)
            tank.__osc_events = {}
            tank.OnAnimTerrainCollision = function(self, bone, bx, by, bz)
                table.insert(self.__osc_events, 'down')
            end
            tank.OnNotAnimTerrainCollision = function(self, bone, bx, by, bz)
                table.insert(self.__osc_events, 'up')
            end
            local d = CreateCollisionDetector(tank)
            d:WatchBone(0)
            d:EnableTerrainCheck(true)
            if enable then d:Enable() end
            x = x + 15
            return tank
        end
        __osc_sunk = watched(true)
        __osc_never = watched(false)
    )");
    run(5);
    lua("setup: both sink below the ground", R"(
        for _, tank in {__osc_sunk, __osc_never} do
            local p = tank:GetPosition()
            Warp(tank, {p[1], GetTerrainHeight(p[1], p[3]) - 3, p[3]})
        end
    )");
    run(1);
    lua("setup: and rise above it", R"(
        for _, tank in {__osc_sunk, __osc_never} do
            local p = tank:GetPosition()
            Warp(tank, {p[1], GetTerrainHeight(p[1], p[3]) + 3, p[3]})
        end
    )");
    run(3);
    lua("Test 5: a bone sinking below the surface and back is told once each way", R"(
        local e = table.concat(__osc_sunk.__osc_events, ' ')
        if e ~= 'down up' then error('events: [' .. e .. ']') end
    )");
    lua("Test 6: a detector never enabled tells nothing", R"(
        if table.getn(__osc_never.__osc_events) ~= 0 then
            error('events: ' .. table.concat(__osc_never.__osc_events, ' '))
        end
    )");

    // A crash: the CZAR's and the Ahwassa's OnKilled watch their bones
    // against the terrain (retail's scripts); each one reaching the ground
    // deals 1000 in 5 there. Each bone is told once, however long it lies.
    // (FAF's CZAR keeps the explosions but no longer deals that damage, so
    // Test 8 holds of retail's data, as the gate runs it.)
    lua("setup: a CZAR and an Ahwassa in flight, killed", R"(
        __osc_crashes = {}
        local function flier(bp, dx)
            local x, z = __osc_ground[1] + dx, __osc_ground[2] + 50
            local u = CreateUnitHPR(bp, 'ARMY_1', x, GetTerrainHeight(x, z) + 25, z, 0, 0, 0)
            local rec = {unit = u, bones = {}, at = {}, hits = {}}
            local crash = u.OnAnimTerrainCollision
            u.OnAnimTerrainCollision = function(self, bone, bx, by, bz)
                rec.bones[bone] = (rec.bones[bone] or 0) + 1
                table.insert(rec.at, {bx, by, bz})
                return crash(self, bone, bx, by, bz)
            end
            table.insert(__osc_crashes, rec)
        end
        flier('uaa0310', -45)
        flier('xsa0402', 45)
        -- Its script's damage, where each bone lands: 1000 in 5.
        local damage_area = DamageArea
        DamageArea = function(instigator, pos, radius, amount, kind, friendly, selfdamage)
            for _, rec in __osc_crashes do
                if instigator == rec.unit and amount == 1000 and radius == 5 then
                    table.insert(rec.hits, {pos[1], pos[2], pos[3]})
                end
            end
            return damage_area(instigator, pos, radius, amount, kind, friendly, selfdamage)
        end
    )");
    run(20);
    lua("setup: both are shot down", R"(
        for _, rec in __osc_crashes do rec.unit:Kill() end
    )");
    run(200);
    lua("Test 7: a crashing flier's bones are told on reaching the ground, each once", R"(
        for _, rec in __osc_crashes do
            local told = 0
            for bone, n in rec.bones do
                if n ~= 1 then error(bone .. ' was told ' .. n .. ' times') end
                told = told + 1
            end
            if told == 0 then error('no bone of the crashing flier was told') end
            LOG('Footfall test: ' .. told .. ' bones met the ground')
        end
    )");
    lua("Test 8: each landing deals its script's crash damage there, on the ground", R"(
        for _, rec in __osc_crashes do
            if table.getn(rec.hits) ~= table.getn(rec.at) then
                error(table.getn(rec.at) .. ' landings, ' .. table.getn(rec.hits) .. ' crash damages')
            end
            for i, p in rec.at do
                local h = rec.hits[i]
                if math.abs(h[1] - p[1]) > 0.01 or math.abs(h[3] - p[3]) > 0.01 then
                    error('a crash damage away from its landing')
                end
                -- Told the tick it is first under the surface: at most a
                -- tick's fall below it.
                local under = GetSurfaceHeight(p[1], p[3]) - p[2]
                if under < -0.01 or under > 5 then
                    error('a landing ' .. under .. ' under the ground')
                end
            end
        end
    )");

    spdlog::info("Footfall test: {} passed, {} failed", t.pass, t.fail);
}

} // namespace osc::test
