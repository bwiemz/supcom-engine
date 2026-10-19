#include "app/self_destruct_flow_test.hpp"

#include "core/test_status.hpp"
#include "lua/lua_state.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

namespace osc::app {

namespace {

/// Frames to reach the end before the run counts as stuck.
constexpr u32 kMaxFrames = 20000;
/// The player's army (0-based).
constexpr i32 kPlayerArmy = 1;

/// The front end's skirmish, as --auto-skirmish's, with the AI in the first
/// slot and the player in the second.
constexpr const char* kLaunch = R"(
    local scenario = '/maps/SCMP_009/SCMP_009_scenario.lua'
    LaunchSinglePlayerSession({
        GameOptions = { ScenarioFile = scenario },
        PlayerOptions = {
            [1] = { Human = false, PlayerName = 'AI', AIPersonality = 'adaptive',
                    Faction = 1, Team = 2, StartSpot = 1 },
            [2] = { Human = true, PlayerName = 'Player', Faction = 1, Team = 1, StartSpot = 2 },
        },
    })
)";

} // namespace

u32 self_destruct_commander(lua::LuaState& ui, const sim::SimState& sim, i32 army,
                            std::string& why) {
    u32 acu = 0;
    sim.entity_registry().for_each_unit([&](const sim::Entity& e) {
        if (acu == 0 && e.army() == army && e.blueprint_id().ends_with("l0001"))
            acu = e.entity_id();
    });
    if (acu == 0) {
        why = fmt::format("army {} has no commander", army);
        return 0;
    }
    const auto ctrl_k =
        fmt::format("SelectUnits({{GetUnitById({})}}) "
                    "import('/lua/ui/game/confirmunitdestroy.lua').ConfirmUnitDestruction()",
                    acu);
    if (auto r = ui.do_string(ctrl_k); !r) {
        why = "self-destructing: " + r.error().message;
        return 0;
    }
    return acu;
}

bool SelfDestructFlowTest::start(lua::LuaState& ui) {
    spdlog::info("=== self-destruct flow test: the player on ARMY_2 self-destructs ===");
    if (auto r = ui.do_string(kLaunch); !r) {
        fail("launching: " + r.error().message);
        return false;
    }
    return true;
}

void SelfDestructFlowTest::frame(lua::LuaState& ui, const sim::SimState* sim) {
    if (done_) return;
    if (++frames_ > kMaxFrames) {
        done_ = true; // stuck: reported by finish
        return;
    }
    if (!sim) return;
    const u32 tick = sim->tick_count();
    if (acu_ == 0 && tick >= kSelfDestructTick) {
        if (sim->focus_army() != kPlayerArmy) {
            fail(fmt::format("the game focuses army {}, not the player's", sim->focus_army()));
            return;
        }
        std::string why;
        acu_ = self_destruct_commander(ui, *sim, kPlayerArmy, why);
        if (acu_ == 0) {
            fail(why);
            return;
        }
        spdlog::info("self-destruct-flow: commander {} self-destructs at tick {}", acu_, tick);
    }
    if (acu_ != 0 && (tick >= kEndTick || sim->game_over())) {
        const auto* acu = sim->entity_registry().find(acu_);
        if (acu && !acu->destroyed() && !static_cast<const sim::Unit*>(acu)->is_dying()) {
            fail(fmt::format("the commander still lives at tick {}", tick));
            return;
        }
        end_tick_ = tick;
        done_ = true;
    }
}

void SelfDestructFlowTest::fail(const std::string& why) {
    test_status::fail("[FAIL] self-destruct-flow: {}", why);
    done_ = true;
}

void SelfDestructFlowTest::finish() const {
    if (end_tick_ != 0) {
        spdlog::info("[PASS] self-destruct-flow: played to tick {}", end_tick_);
    } else if (test_status::failure_count() == 0) {
        test_status::fail("[FAIL] self-destruct-flow: the game never reached its end ({} frames)",
                          frames_);
    }
}

} // namespace osc::app
