#pragma once

// What the application's own files share (M192 step 2): app.cpp's run() and
// the files split from it. Not for the tests, which use support.hpp.

#include "app/app.hpp"
#include "app/support.hpp"
#include "audio/sound_manager.hpp"
#include "blueprints/blueprint_store.hpp"
#include "core/front_end_data.hpp"
#include "core/game_state.hpp"
#include "core/localization.hpp"
#include "core/preferences.hpp"
#include "core/tick_clock.hpp"
#include "lua/beat_system.hpp"
#include "lua/init_loader.hpp"
#include "lua/lua_state.hpp"
#include "lua/scenario_loader.hpp"
#include "lua/special_files.hpp"
#include "sim/game_setup.hpp"
#include "sim/replay.hpp"
#include "sim/saved_game.hpp"
#include "sim/sim_state.hpp"
#include "sim/thread_manager.hpp"
#include "sim/world_snapshot.hpp"
#include "ui/keymap.hpp"
#include "ui/ui_control.hpp"
#include "ui/wld_ui_provider.hpp"
#include "vfs/virtual_file_system.hpp"

#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <set>
#include <string>

namespace osc::lua {
class SmokeTestHarness;
}
namespace osc::sim {
class SimRandom;
}

namespace osc::app {

/// The command line, as the run reads it (M192 step 2b).
struct Options {
    std::string map_path;                      ///< --map, or the replay's scenario
    u32 tick_count = 0;                        ///< --ticks (0: not given)
    std::optional<sim::Replay> replay_to_play; ///< --replay
    /// --load <file> in a headless run: the saved game, which catches up to
    /// its saved tick and plays on.
    std::optional<sim::SavedGame> save_to_load;
    /// --load <file> otherwise: the window opens it as retail's Load dialog
    /// does (LoadSavedGame).
    std::string load_path;
    std::string save_path; ///< --save <file>: the game, saved at --save-at's tick
    u32 save_at = 0;       ///< --save-at <tick>
    bool scripted_orders = false;
    std::string watch_path; ///< --watch
    bool replay_flow_test = false;
    bool load_flow_test = false;
    /// A scripted run of the windowed loop: offscreen, silent, fixed clock.
    bool scripted_window = false;
    bool no_fog = false;
    bool legacy_hud = false;
    bool no_decals = false;
    bool profile_enabled = false;
    bool ai_skirmish = false;
    bool instrument = false;
    bool builder_debug = false;
    std::string ai_personality;
    size_t ai_army_count = 2;           ///< --ai-armies
    std::set<std::string> cmdline_args; ///< for HasCommandLineArg
    /// A checked run: headless, and its exit code is the checks' result.
    bool any_test = false;
    bool headless = false;
    /// A capture or scripted window: no audio output.
    bool silent_capture = false;
    std::string seed_arg; ///< --seed
    /// A run that must repeat: a fixed seed unless --seed says otherwise.
    bool reproducible_run = false;
    /// A player's game: their preferences, user folder and LastGame.
    bool interactive = false;
};

// cli.cpp
void print_usage();
lua::InitConfig parse_args(int argc, char* argv[], const TestModes* tests);
u32 parse_ticks_arg(int argc, char* argv[]);
std::string parse_map_arg(int argc, char* argv[]);
/// The test mode flag on the command line, if any.
const char* test_mode_flag(int argc, char* argv[]);
/// The run's options, or null when the --replay or --load file can't be
/// read.
std::optional<Options> parse_options(int argc, char* argv[], const TestRequest& request);

// session.cpp
void attach_sound(lua::LuaState& sim_lua, sim::SimState& sim, audio::SoundManager* sound);
extern std::ofstream* g_checksum_trace;
extern std::ofstream* g_entity_trace;
extern u32 g_entity_trace_from;
extern u32 g_entity_trace_to;
extern std::ofstream* g_rng_trace;
extern u32 g_rng_trace_from;
extern u32 g_rng_trace_to;
extern std::string g_record_path;
bool write_recording(const sim::SimState& sim, const std::string& path);
std::optional<sim::Replay> load_replay(const std::string& path);
void issue_scripted_orders(sim::SimState& sim, sim::SimRandom& rng, u32 tick);
void issue_order_before_save(sim::SimState& sim);
int play_replay(sim::SimState& sim, const sim::Replay& replay);
u64 launch_seed(const std::string& seed_arg, bool reproducible);

/// The world as it is drawn: the sim's last two ticks, and how far the frame
/// is between them. Every tick lands here, whoever runs it (the loop, the
/// lockstep session), so interpolation follows the ticks that really came.
struct WorldInterp {
    osc::sim::WorldHistory history;
    osc::TickClock clock{osc::sim::SimState::SECONDS_PER_TICK};

    /// Follow a new sim's ticks from its first one. Must outlive `sim`.
    void attach(osc::sim::SimState& sim) {
        history.clear();
        clock.reset();
        sim.set_tick_observer([this](const osc::sim::SimState& s) {
            history.capture(s);
            clock.on_tick();
        });
    }

    osc::sim::FrameView view() const {
        return {&history.prev(), &history.cur(), clock.alpha()};
    }
};

// frames.cpp
bool mouse_over_ui(lua_State* uiL, f64 x, f64 y);
void cancel_command_mode(lua_State* uiL);
void sync_build_ghost(sim::SimState& sim, const renderer::CommandMode& m, bool& ghost_from_mode);
void lan_launch_session(lua_State* uL, const std::string& scenario);

// ui_globals.cpp
/// Register the session's UI globals (FlushEvents, SessionIsReplay, ...) on
/// the UI state.
void register_session_ui_globals(lua::LuaState& ui_lua_state);

/// One run of the engine: what run() builds and its phases over it (M192
/// step 2b). The members are declared in the order the run makes them, so
/// they are destroyed in the reverse of it.
class App {
public:
    App(int arg_count, char* arg_values[], TestModes* test_modes, lua::InitConfig init_config,
        TestRequest test_request, Options options);
    ~App();
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    /// Boot, then the windowed loop or the headless run: the exit code.
    int run();

private:
    // boot.cpp. A value ends the run.
    std::optional<int> boot();
    /// The sim Lua state, the blueprints, audio and the traces.
    std::optional<int> boot_engine();
    /// The game's sim, with --map.
    std::optional<int> boot_game();
    /// The UI state, up to the game's interface or the front end.
    std::optional<int> boot_ui();
    /// The session, the UI's registries, and the test modes' front end.
    std::optional<int> start();

    // window.cpp. A value ends the run.
    std::optional<int> run_window();

    // headless.cpp
    int run_headless();
    /// After a headless tick: a loaded game (--load) is checked against its
    /// save until the player's turn, and --save-at's tick saves the game.
    /// False once a loaded game stops matching its save.
    bool after_headless_tick();
    /// After a tick of a loaded game catching up: check it against the save,
    /// and let catch_up go once the player has the game. False (logged) on
    /// the first tick that differs.
    bool check_catch_up();

    /// FA's LastGame: the game just left, as recorded, in the current
    /// profile's replays -- when a new game starts, on the way back to the
    /// lobby, and at exit. Interactive games only; a replay isn't recorded.
    void save_last_game();

    int argc;
    char** argv;
    TestModes* tests;
    const lua::InitConfig config;
    const TestRequest request;
    Options opt; // not const: the UI registry holds its cmdline_args

    std::unique_ptr<lua::LuaState> sim_lua_state;
    vfs::VirtualFileSystem vfs;
    lua::InitLoader loader;
    blueprints::BlueprintStore store;
    audio::SoundManager sound;
    std::ofstream checksum_trace;
    std::ofstream entity_trace;
    std::ofstream rng_trace;

    WorldInterp world_interp; // outlives every sim it observes
    std::unique_ptr<sim::SimState> sim_state;
    lua::ScenarioMetadata scenario_meta;
    /// The game's setup: a replay's or saved game's own, or the command
    /// line's.
    sim::GameSetup game_setup;
    /// A loaded game's save (--load, LoadSavedGame), while the game catches
    /// up with it: checked tick by tick until the player takes over.
    std::optional<sim::ReplayPlayback> catch_up;
    /// --record: the last game's replay is written as the run ends, however
    /// it ends (the normal end writes it before logging shuts down).
    struct RecordingWriter {
        std::unique_ptr<sim::SimState>& sim;
        bool written = false;
        void write() {
            if (written) return;
            written = true;
            if (!g_record_path.empty() && sim && sim->recording())
                write_recording(*sim, g_record_path);
        }
        ~RecordingWriter() { write(); }
    } recording_writer{sim_state};

    lua::LuaState ui_lua_state;
    blueprints::BlueprintStore ui_store;
    core::Localization loc_cache;
    core::Preferences prefs;
    /// Replays and saved games (FA's special files).
    std::filesystem::path user_dir;
    std::filesystem::path temp_user_dir;
    struct TempDirRemover {
        std::filesystem::path dir;
        ~TempDirRemover() {
            std::error_code ec;
            if (!dir.empty()) std::filesystem::remove_all(dir, ec);
        }
    } temp_user_dir_remover;
    std::optional<lua::SpecialFiles> special_files;
    ui::WldUIProvider wld_provider;
    ui::UIControlRegistry ui_registry;
    sim::ThreadManager ui_thread_manager;
    u32 ui_frame_count = 0;
    lua::BeatFunctionRegistry beat_registry;
    ui::KeyMapRegistry keymap_registry;
    FrontEndData front_end_data;
    GameStateManager game_state_mgr;
    /// What a test mode drives.
    Engine engine;
    std::unique_ptr<lua::SmokeTestHarness> instrument_harness;
};

} // namespace osc::app
