#include "app/mods_flow_test.hpp"

#include "core/test_status.hpp"
#include "lua/lua_state.hpp"
#include "sim/sim_state.hpp"

#include <spdlog/spdlog.h>

#include <cstring>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace osc::app {

namespace {

/// Frames to reach the report before the run counts as stuck.
constexpr u32 kMaxFrames = 20000;

/// The front end's skirmish (as --auto-skirmish's), with the game mods
/// retail's lobby would launch: Mods.GetGameMods() of the player's
/// selection. The AI plays UEF too: the test is about the mods.
constexpr const char* kLaunch = R"(
    local scenario = '/maps/SCMP_009/SCMP_009_scenario.lua'
    LaunchSinglePlayerSession({
        GameOptions = { ScenarioFile = scenario },
        PlayerOptions = {
            [1] = { Human = true, PlayerName = 'Player', Faction = 1, Team = 1, StartSpot = 1 },
            [2] = { Human = false, PlayerName = 'AI', AIPersonality = 'adaptive',
                    Faction = 1, Team = 2, StartSpot = 2 },
        },
        GameMods = import('/lua/mods.lua').GetGameMods(),
    })
)";

/// What a state's mods made of it, on one line.
constexpr const char* kReport = R"(
    local uids = {}
    for i, m in ipairs(__active_mods) do table.insert(uids, tostring(m.uid)) end
    local bps = rawget(_G, '__blueprints')
    local acu = bps and bps.uel0001
    local economy = acu and acu.Economy or {}
    local general = acu and acu.General or {}
    return 'mods=' .. table.concat(uids, ',')
        .. ' mass=' .. tostring(economy.ProductionPerSecondMass)
        .. ' energy=' .. tostring(economy.ProductionPerSecondEnergy)
        .. ' name=' .. tostring(general.UnitName)
        .. ' probe=' .. tostring(rawget(_G, 'OscModsFlowProbe'))
)";

} // namespace

bool ModsFlowTest::start(lua::LuaState& ui, bool launch) {
    spdlog::info("=== mods flow test: {} ===",
                 launch ? "a skirmish with the player's mods" : "a recorded game's mods");
    if (!launch) return true;
    if (auto r = ui.do_string(kLaunch); !r) {
        test_status::fail("[FAIL] mods-flow: launching: {}", r.error().message);
        done_ = true;
        return false;
    }
    return true;
}

void ModsFlowTest::frame(lua::LuaState& ui, lua::LuaState* sim_lua, const sim::SimState* sim) {
    if (done_) return;
    if (++frames_ > kMaxFrames) {
        done_ = true; // stuck: reported by finish
        return;
    }
    if (!sim || !sim_lua || sim->tick_count() < kReportTick) return;
    spdlog::info("mods-flow: sim: {}", report(sim_lua->raw()));
    spdlog::info("mods-flow: ui: {}", report(ui.raw()));
    reported_ = true;
    done_ = true;
}

void ModsFlowTest::finish() const {
    if (!reported_) {
        test_status::fail("[FAIL] mods-flow: the game never reached tick {} ({} frames)",
                          kReportTick, frames_);
        return;
    }
    spdlog::info("[PASS] mods-flow: reported at tick {}", kReportTick);
}

std::string ModsFlowTest::report(lua_State* L) {
    const int top = lua_gettop(L);
    std::string line;
    if (luaL_loadbuffer(L, kReport, std::strlen(kReport), "=mods-flow") != 0 ||
        lua_pcall(L, 0, 1, 0) != 0) {
        line = std::string("error: ") + (lua_isstring(L, -1) ? lua_tostring(L, -1) : "?");
        test_status::fail("[FAIL] mods-flow: the report: {}", line);
    } else if (lua_type(L, -1) == LUA_TSTRING) {
        line = lua_tostring(L, -1);
    }
    lua_settop(L, top);
    return line;
}

} // namespace osc::app
