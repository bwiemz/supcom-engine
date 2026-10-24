#include "app/loading_screen_test.hpp"

#include "core/test_status.hpp"
#include "lua/lua_state.hpp"
#include "sim/sim_state.hpp"
#include "ui/ui_control.hpp"

#include <spdlog/spdlog.h>

namespace osc::app {

namespace {

/// Frames of the game (not of its loads) before the run counts as stuck.
constexpr u32 kMaxFrames = 20000;

/// The front end's skirmish, as --auto-skirmish's.
constexpr const char* kLaunch = R"(
    LaunchSinglePlayerSession({
        GameOptions = { ScenarioFile = '/maps/SCMP_009/SCMP_009_scenario.lua' },
        PlayerOptions = {
            [1] = { Human = true, PlayerName = 'Player', Faction = 1, Team = 1, StartSpot = 1 },
            [2] = { Human = false, PlayerName = 'AI', AIPersonality = 'adaptive',
                    Faction = 2, Team = 2, StartSpot = 2 },
        },
    })
)";

/// The game saved in the current profile's saves, then loaded as the Load
/// dialog loads it.
constexpr const char* kSaveAndLoad = R"(
    local profile = import('/lua/user/prefs.lua').GetCurrentProfile()
    profile = profile and profile.Name or 'Player'
    local data = GetSpecialFiles('SaveGame')
    local file = data.directory .. profile .. '/loading.' .. data.extension
    local result
    InternalSaveGame(file, 'loading', function(worked, errmsg)
        result = {worked = worked, errmsg = errmsg}
    end)
    if not (result and result.worked) then
        error('InternalSaveGame: ' .. tostring(result and result.errmsg))
    end
    local worked, err, detail = LoadSavedGame(file)
    if not worked then error('LoadSavedGame: ' .. tostring(err) .. ' ' .. tostring(detail)) end
)";

/// Whether a playing movie's frame was drawn: the UI's draw marks each
/// movie on screen or not by whether it drew its quad (UiRenderer::update).
bool movie_drawn(const ui::UIControlRegistry& registry) {
    for (const auto& ptr : registry.all()) {
        const ui::UIControl* ctrl = ptr.get();
        if (ctrl && !ctrl->destroyed() &&
            ctrl->control_type() == ui::UIControl::ControlType::Movie && ctrl->movie_playing() &&
            ctrl->movie_on_screen())
            return true;
    }
    return false;
}

} // namespace

bool LoadingScreenTest::start(lua::LuaState& ui) {
    spdlog::info("=== loading screen test: a skirmish's load, then its saved game's ===");
    if (auto r = ui.do_string(kLaunch); !r) {
        fail("launching: " + r.error().message);
        return false;
    }
    return true;
}

void LoadingScreenTest::loading_frame_drawn(const ui::UIControlRegistry& registry, bool restoring) {
    Load& load = loads_[static_cast<size_t>(phase_)];
    const bool movie = movie_drawn(registry);
    ++load.frames;
    load.movie_frames += movie ? 1 : 0;
    if (restoring) {
        ++load.restore_frames;
        load.restore_movie_frames += movie ? 1 : 0;
    }
}

void LoadingScreenTest::frame(lua::LuaState& ui, const sim::SimState* sim, bool interface_up,
                              std::optional<u32> restored_at) {
    if (done_) return;
    if (++frames_ > kMaxFrames) {
        done_ = true; // stuck: reported by finish
        return;
    }
    if (!sim || !interface_up) return;
    const u32 tick = sim->tick_count();
    if (phase_ == 0) {
        if (tick < kSaveTick) return;
        // (the launch it asks for loads at the end of this frame)
        if (auto r = ui.do_string(kSaveAndLoad); !r) {
            fail("saving and loading: " + r.error().message);
            return;
        }
        spdlog::info("loading-screen: the skirmish saved at tick {}; loading it", tick);
        phase_ = 1;
        return;
    }
    if (!restored_at) {
        fail("the saved game caught up from its history: its snapshot wasn't restored");
        return;
    }
    if (tick >= *restored_at + kPlayOn) {
        restored_at_ = restored_at;
        done_ = true;
    }
}

void LoadingScreenTest::fail(const std::string& why) {
    test_status::fail("[FAIL] loading-screen: {}", why);
    done_ = true;
}

void LoadingScreenTest::finish() const {
    const Load& skirmish = loads_[0];
    const Load& saved = loads_[1];
    if (skirmish.movie_frames == 0)
        test_status::fail("[FAIL] loading-screen: the skirmish's load drew {} frames, none with "
                          "its loading movie",
                          skirmish.frames);
    if (phase_ == 1 && saved.restore_movie_frames == 0)
        test_status::fail("[FAIL] loading-screen: the saved game's snapshot restored over {} "
                          "frames, none with the loading movie ({} with it as its world loaded)",
                          saved.restore_frames, saved.movie_frames);
    if (!restored_at_) {
        if (test_status::failure_count() == 0)
            test_status::fail("[FAIL] loading-screen: the saved game never played on ({} frames, "
                              "phase {})",
                              frames_, phase_);
        return;
    }
    if (test_status::failure_count() == 0)
        spdlog::info("[PASS] loading-screen: the skirmish's load drew {} frames, {} with its "
                     "movie; its saved game's {}, {} with the movie, {} of them as its snapshot "
                     "restored (tick {})",
                     skirmish.frames, skirmish.movie_frames, saved.frames, saved.movie_frames,
                     saved.restore_movie_frames, *restored_at_);
}

} // namespace osc::app
