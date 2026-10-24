#include "app/restart_flow_test.hpp"

#include "core/test_status.hpp"
#include "lua/lua_state.hpp"
#include "sim/sim_state.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

namespace osc::app {

namespace {

/// Frames to reach the end before the run counts as stuck.
constexpr u32 kMaxFrames = 20000;
/// Frames for the restart to start the new game.
constexpr u32 kMaxRestartFrames = 600;

/// The front end's skirmish, as --auto-skirmish's: the player and an AI.
constexpr const char* kLaunch = R"(
    local scenario = '/maps/SCMP_009/SCMP_009_scenario.lua'
    LaunchSinglePlayerSession({
        GameOptions = { ScenarioFile = scenario },
        PlayerOptions = {
            [1] = { Human = true, PlayerName = 'Player', Faction = 1, Team = 1, StartSpot = 1 },
            [2] = { Human = false, PlayerName = 'AI', AIPersonality = 'adaptive',
                    Faction = 2, Team = 2, StartSpot = 2 },
        },
    })
)";

/// The game menu's Restart Game, confirmed: retail's tabs.lua RestartGame
/// dialog's Yes. The session says first that it can be.
constexpr const char* kRestart = R"(
    if SessionCanRestart() ~= true then error('SessionCanRestart() is not true') end
    RestartSession()
)";

} // namespace

bool RestartFlowTest::start(lua::LuaState& ui) {
    spdlog::info("=== restart flow test: a skirmish restarted from its game menu ===");
    if (auto r = ui.do_string(kLaunch); !r) {
        fail("launching: " + r.error().message);
        return false;
    }
    return true;
}

void RestartFlowTest::frame(lua::LuaState& ui, const sim::SimState* sim) {
    if (done_) return;
    if (++frames_ > kMaxFrames) {
        done_ = true; // stuck: reported by finish
        return;
    }
    if (!sim) return;
    const u32 tick = sim->tick_count();
    if (!restarted_) {
        if (tick < kRestartTick) return;
        first_game_ = sim::SimState::sim_generation();
        first_seed_ = sim->seed();
        first_scenario_ = sim->recorded_replay().setup.scenario;
        first_armies_ = sim->army_count();
        if (auto r = ui.do_string(kRestart); !r) {
            fail("restarting: " + r.error().message);
            return;
        }
        restarted_ = true;
        spdlog::info("restart-flow: RestartSession() at tick {}", tick);
        return;
    }
    if (sim::SimState::sim_generation() == first_game_) {
        // The window takes the request between frames
        if (++frames_since_restart_ > kMaxRestartFrames)
            fail(fmt::format("the game never restarted ({} frames)", frames_since_restart_));
        return;
    }
    if (!restart_seen_) {
        restart_seen_ = true;
        const std::string scenario = sim->recorded_replay().setup.scenario;
        if (tick >= kRestartTick) {
            fail(fmt::format("the new game began at tick {}, not from its start", tick));
        } else if (sim->playback() || sim->loaded_from_save()) {
            fail("the new game is a replay or a load, not the game started again");
        } else if (scenario != first_scenario_ || sim->army_count() != first_armies_) {
            fail(fmt::format("the new game is {} with {} armies, not {} with {}", scenario,
                             sim->army_count(), first_scenario_, first_armies_));
        } else if (sim->seed() != first_seed_) {
            fail(fmt::format("the new game's seed is {:#x}, not its launch's {:#x}", sim->seed(),
                             first_seed_));
        } else {
            spdlog::info("restart-flow: the game started again at tick {}", tick);
        }
        return;
    }
    if (tick >= kEndTick || sim->game_over()) {
        if (auto r = ui.do_string("if not SessionIsActive() then error('no session') end"); !r) {
            fail("after the restart: " + r.error().message);
            return;
        }
        end_tick_ = tick;
        done_ = true;
    }
}

void RestartFlowTest::fail(const std::string& why) {
    test_status::fail("[FAIL] restart-flow: {}", why);
    done_ = true;
}

void RestartFlowTest::finish() const {
    if (end_tick_ != 0 && test_status::failure_count() == 0) {
        spdlog::info("[PASS] restart-flow: restarted, and played on to tick {}", end_tick_);
    } else if (test_status::failure_count() == 0) {
        test_status::fail("[FAIL] restart-flow: the game never reached its end ({} frames)",
                          frames_);
    }
}

} // namespace osc::app
