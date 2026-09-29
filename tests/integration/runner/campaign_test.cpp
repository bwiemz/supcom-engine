// --campaign-test (M209): FA's campaign, launched as retail's
// SetupCampaignSession launches an operation. The first (X1CA_001, "Black
// Day") boots with its first army the player's and the rest the campaign's
// AI, the campaign's options, its armies' factions and alliances from its
// save file, and then runs a minute of its script: a script error anywhere
// in it fails the run (test modes count Lua errors).

#include "integration_tests.hpp"
#include "render_probe.hpp"

#include "lua/lua_state.hpp"
#include "sim/army_brain.hpp"
#include "sim/sim_state.hpp"

#include <spdlog/spdlog.h>

#include <string>

namespace osc::test {

void test_campaign(TestContext& ctx) {
    spdlog::info("=== CAMPAIGN TEST: X1CA_001 boots and runs (M209) ===");
    Tally t;
    const auto lua_check = [&](const char* what, const char* code) {
        const auto ran = ctx.lua_state.do_string(code);
        t.check(static_cast<bool>(ran),
                ran ? std::string(what) : std::string(what) + ": " + ran.error().message);
    };

    lua_check("Test 1: five armies; the first the player's, the rest the campaign's AI", R"(
        if table.getn(ArmyBrains) ~= 5 then error(table.getn(ArmyBrains) .. ' armies') end
        if ArmyBrains[1].BrainType ~= 'Human' then error('the player: ' .. tostring(ArmyBrains[1].BrainType)) end
        for i = 2, 5 do
            if ArmyBrains[i].BrainType ~= 'AI' then error(i .. ': ' .. tostring(ArmyBrains[i].BrainType)) end
        end
    )");
    lua_check("Test 2: the campaign's options (SetupCampaignSession's)", R"(
        local o = ScenarioInfo.Options
        if o.Difficulty ~= 2 or o.FogOfWar ~= 'explored' or o.Timeouts ~= -1 or
           o.GameSpeed ~= 'normal' or o.Victory ~= 'sandbox' or not o.DoNotShareUnitCap then
            error(string.format('Difficulty %s, FogOfWar %s, Timeouts %s, GameSpeed %s, Victory %s',
                tostring(o.Difficulty), tostring(o.FogOfWar), tostring(o.Timeouts),
                tostring(o.GameSpeed), tostring(o.Victory)))
        end
    )");
    // The save file's factions are 0-based (UEF 0, Aeon 1, Seraphim 3), as
    // SetArmyFactionIndex takes them; GetFactionIndex answers 1-based.
    lua_check("Test 3: each army's faction from the save file", R"(
        local want = {Player = 1, Seraphim = 4, Order = 2, UEF = 1}
        for name, index in want do
            local got = GetArmyBrain(name):GetFactionIndex()
            if got ~= index then error(name .. ': ' .. tostring(got)) end
        end
        SetArmyFactionIndex('Civilians', 2)
        if GetArmyBrain('Civilians'):GetFactionIndex() ~= 3 then error('SetArmyFactionIndex is 0-based') end
    )");
    // Two draws of the sim's generator, within the middle 80% of the map.
    lua_check("Test 4: GenerateArmyStart picks a start inside the map's middle", R"(
        GenerateArmyStart(5)
        local x, z = GetArmyBrain('Civilians'):GetArmyStartPos()
        local w, h = GetMapSize()
        if not (x >= 0.1 * w and x <= 0.9 * w and z >= 0.1 * h and z <= 0.9 * h) then
            error(string.format('(%.0f, %.0f) on a %d x %d map', x, z, w, h))
        end
    )");

    for (int i = 0; i < 600; ++i) ctx.sim.tick();

    // The operation's first playable area is set, and every army but the
    // player's ignores it (its script's SetIgnorePlayableRect).
    t.check(ctx.sim.has_playable_rect() && ctx.sim.army_at(0) &&
                !ctx.sim.army_at(0)->use_whole_map() && ctx.sim.army_at(1) &&
                ctx.sim.army_at(1)->use_whole_map(),
            "Test 5: the playable area is set, and the other armies ignore it");
    lua_check("Test 6: a minute in, the mission's script is under way", R"(
        if not ScenarioInfo.MapData.PlayableRect then error('no playable area') end
        if GetGameTick() < 600 then error('tick ' .. GetGameTick()) end
    )");
    t.check(ctx.sim.entity_registry().count() > 1000,
            "Test 7: the operation's armies are on the map (" +
                std::to_string(ctx.sim.entity_registry().count()) + " entities)");

    // Only the first army's view changes (Moho's SetAllianceOneWay). From
    // allies both ways, then back to how the operation had them.
    lua_check("Test 8: SetAllianceOneWay sets one army's view of another", R"(
        local function view(a, b)
            if IsAlly(a, b) then return 'Ally' elseif IsEnemy(a, b) then return 'Enemy' end
            return 'Neutral'
        end
        local was45, was54 = view(4, 5), view(5, 4)
        SetAlliance(4, 5, 'Ally')
        SetAllianceOneWay(4, 5, 'Enemy')
        local ok = IsEnemy(4, 5) and IsAlly(5, 4)
        SetAllianceOneWay(4, 5, was45)
        SetAllianceOneWay(5, 4, was54)
        if not ok then error('4 sees 5 as ' .. view(4, 5) .. ', 5 sees 4 as ' .. view(5, 4)) end
        if view(4, 5) ~= was45 or view(5, 4) ~= was54 then error('not restored') end
    )");

    spdlog::info("Campaign test: {}/{} passed", t.pass, t.pass + t.fail);
}

} // namespace osc::test
