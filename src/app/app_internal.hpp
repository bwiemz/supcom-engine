#pragma once

// What the application's own files share (M192 step 2): app.cpp's run() and
// the files split from it. Not for the tests, which use support.hpp.

#include "app/app.hpp"
#include "app/bench.hpp"
#include "app/window_commands.hpp"
#include "app/match_telemetry.hpp"
#include "app/support.hpp"
#include "app/scripted_mouse.hpp"
#include "app/ui_clicks.hpp"
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
#include "lua/sim_sync.hpp"
#include "lua/special_files.hpp"
#include "lua/url_bindings.hpp"
#include "renderer/range_overlays.hpp"
#include "sim/game_setup.hpp"
#include "sim/replay.hpp"
#include "sim/saved_game.hpp"
#include "sim/sim_state.hpp"
#include "sim/thread_manager.hpp"
#include "sim/world_snapshot.hpp"
#include "ui/console.hpp"
#include "ui/keymap.hpp"
#include "ui/ui_control.hpp"
#include "ui/wld_ui_provider.hpp"
#include "vfs/virtual_file_system.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

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
    /// --load <file> in a headless run: the saved game, which is restored
    /// from its snapshot (or catches up to its saved tick) and plays on.
    std::optional<sim::SavedGame> save_to_load;
    /// --load-by-replay: catch up from the history even when the save has a
    /// snapshot (M208c: the oracle a restore is checked against).
    bool load_by_replay = false;
    /// --load <file> otherwise: the window opens it as retail's Load dialog
    /// does (LoadSavedGame).
    std::string load_path;
    std::string save_path; ///< --save <file>: the game, saved at --save-at's tick
    u32 save_at = 0;       ///< --save-at <tick>
    /// --save-at <tick,tick,...> in a windowed game (--fast-match, --play-for):
    /// the game saved after each, `{tick}` in --save's path naming it (the
    /// Performance Lab's scenarios: a match at 20, 40 and 60 minutes)
    std::vector<u32> save_ticks;
    /// --post-load-at <tick>: the game schedules a post-load after that tick
    /// (after a save there), as a load at it would -- the save/load oracle's
    /// like for like (docs/plans/2026-10-05-campaign-saves-design.md).
    u32 post_load_at = 0;
    bool scripted_orders = false;
    std::string bench_report; ///< --bench <file>: the run's tick times (M223), and its profile
    /// --bench-scenario <name>: what the --bench run is (a Performance Lab
    /// scenario's name), for its report
    std::string bench_scenario;
    /// --match-telemetry <file>: the match's statistics, as JSON Lines
    std::string match_telemetry_path;
    /// --render-bench <file> (M223b): a saved game's scene rendered, its
    /// frames' figures written; --render-scene, --render-frames, --render-warmup,
    /// --gpu-passes.
    std::string render_bench_report;
    /// --fast-match <ticks>: the windowed loop offscreen and unpaced, for a
    /// game started as a player's is (a launcher's /hostgame, retail's
    /// lobby): once in the game, a sim tick each frame and a frame drawn
    /// now and then, until the sim has run this many ticks. 0: off.
    u32 fast_match_ticks = 0;
    std::string render_scene = "battle";
    u32 render_frames = 600;
    u32 render_warmup = 120;
    bool gpu_passes = false;
    std::string watch_path; ///< --watch
    bool replay_flow_test = false;
    bool load_flow_test = false;
    bool mods_flow_test = false;
    bool self_destruct_flow_test = false;
    bool campaign_flow_test = false; ///< (M209b)
    bool tutorial_flow_test = false; ///< its tutorial route (campaign_flow_test is set too)
    bool outro_flow_test = false;    ///< its outro route (campaign_flow_test is set too)
    bool mods_flow_lobby = false;
    std::vector<UiClick> clicks; ///< --click <label>, --click-at <x>,<y>, in order
    std::vector<MouseStep> mouse; ///< --mouse, in order
    /// --lan-game-host / --lan-game-join <address>, on --mp-port: retail's
    /// LAN lobby played to a game by two processes, offscreen (M218c).
    bool lan_game_host = false;
    std::string lan_game_join;
    u16 lan_game_port = 47624;
    u32 lan_game_quit_at = 0; ///< --lan-game-quit-at: the joiner leaves then (M218e)
    u32 lan_game_self_destruct_at = 0; ///< --lan-game-self-destruct-at: the joiner's Ctrl-K
    std::string lan_game_save;
    std::string lan_game_resume;

    bool sim_thread = true; ///< --no-sim-thread: ticks on the main thread (OE-134)
    bool workers = true;    ///< --no-workers: no worker pool (core/worker_pool.hpp)
    /// --play-for <seconds>: a game played offscreen for this long, its
    /// frames paced as a 60 Hz display's are (the rest of each frame waited
    /// out, as a present waits for the display), and their times logged:
    /// how often the sim's ticks hold a frame back (OE-134). 0: none.
    double play_for = 0;
    /// --play-rate: the game speed --play-for plays at (Moho's sim rate, 0 normal)
    int play_rate = 0;
    /// --play-until-tick: --play-for closes at this game tick, or when the
    /// game is won (a long match's whole run, for its performance). 0: none.
    unsigned play_until_tick = 0;

    bool lan_game_test() const { return lan_game_host || !lan_game_join.empty(); }
    /// `/gpgnet host:port` (Moho's; also --gpgnet): the matchmaking client to
    /// link to, instead of the front end (M220a).
    std::string gpgnet_endpoint;
    /// --gpgnet-scripted: that link's run is a test's: offscreen, counting
    /// script errors, and over once the client closes the link.
    bool gpgnet_scripted = false;
    /// A scripted run of the windowed loop: offscreen, silent, fixed clock.
    bool scripted_window = false;
    bool no_fog = false;
    bool legacy_hud = false;
    bool no_decals = false;
    bool profile_enabled = false;
    /// --profile-frames: the frames' zones only, the sim on its own thread
    bool profile_frames = false;
    bool ai_skirmish = false;
    bool instrument = false;
    bool builder_debug = false;
    std::string ai_personality;
    size_t ai_army_count = 2;           ///< --ai-armies
    /// The command line, in order, for HasCommandLineArg and GetCommandLineArg
    std::vector<std::string> cmdline_args;
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
/// The install to play from: the command line's, else the search's
/// (platform::locate_game_install), with `chosen` as the folder the player
/// chose (else the engine settings').
lua::InitConfig parse_args(int argc, char* argv[], const TestModes* tests,
                           const std::optional<fs::path>& chosen = std::nullopt);
/// The first run (M228a): ask the player where FA is, with native dialogs,
/// and save it. The folder, or nullopt (cancelled, or no dialogs to ask with).
std::optional<fs::path> first_run_find_fa();
u32 parse_ticks_arg(int argc, char* argv[]);
std::string parse_map_arg(int argc, char* argv[]);
/// The test mode flag on the command line, if any.
const char* test_mode_flag(int argc, char* argv[]);
/// The run's options, or null (logged) when the --replay or --load file
/// can't be read, or --save can't be done.
std::optional<Options> parse_options(int argc, char* argv[], const TestRequest& request);
/// A player's own game, which may stop to ask them something (the first
/// run's FA folder, M228a): interactive, and not a scripted window (the
/// replay/load flow tests and LAN games, which run the game binary itself).
bool may_ask_player(const Options& opt);
/// Give the log its file, if --log or the integration runner hasn't (M228b):
/// a player's game logs to the State folder, the last five runs kept, and
/// its crash reports go beside it; any other run logs to opensupcom.log
/// in the working directory, as tests and scripts expect.
void open_run_log(const Options& opt);

// session.cpp
void attach_sound(lua::LuaState& sim_lua, sim::SimState& sim, audio::SoundManager* sound);
extern std::ofstream* g_checksum_trace;
extern std::ofstream* g_entity_trace;
extern u32 g_entity_trace_from;
extern u32 g_entity_trace_to;
extern std::ofstream* g_rng_trace;
extern u32 g_rng_trace_from;
extern u32 g_rng_trace_to;
extern bool g_moho_pathing;
extern bool g_moho_random;
bool recorded_moho_pathing(const sim::Replay& recording);
extern std::string g_record_path;
bool write_recording(const sim::SimState& sim, const std::string& path);
std::optional<sim::Replay> load_replay(const std::string& path);
void issue_scripted_orders(sim::SimState& sim, u32 tick);
void issue_order_before_save(sim::SimState& sim);
int play_replay(sim::SimState& sim, const sim::Replay& replay);
u64 launch_seed(const std::string& seed_arg, bool reproducible);

/// The world as it is drawn: the sim's last two beats, and how far the frame
/// is between them. Every beat lands here, whoever runs it (the loop, the
/// lockstep session), so interpolation follows the ticks that really came.
struct WorldInterp {
    osc::sim::WorldHistory history;
    osc::TickClock clock{osc::sim::SimState::SECONDS_PER_TICK};
    bool one_tick_next = true;
    osc::lua::SyncQueue sync;

    /// The ticks run on the sim's own thread (OE-134): each one's events
    /// are kept aside (WorldHistory::stage_events), the world as the job's
    /// last tick left it captured (stage_world, by the job), and published
    /// between frames (publish). Set only while no tick runs.

    bool staged = false;
    /// Capture on the sim owner; UI Sync delivery runs after collection.
    void attach(osc::sim::SimState& sim, osc::lua::LuaState* ui = nullptr) {
        history.clear();
        clock.reset();
        one_tick_next = true;
        sync.clear();
        sim.set_beat_observer([this, ui](const osc::sim::SimState& s) {
            if (staged) {
                history.stage_events(s);
                if (ui) sync.queue(s.lua_state(), nullptr);

                return;
            }

            history.capture(s);
            note_published();
            if (ui) {
                sync.queue(s.lua_state(), ui->raw());
            }
        });
    }

    /// The sim thread's job done: the world its last tick left published.
    void publish() {
        if (history.publish()) note_published();
    }

    /// The clock told of the newest tick, how far on it is from the one
    /// before, and how far back the history reaches.
    void note_published() {
        const auto& prev = history.prev();
        const auto& cur = history.cur();
        const auto& older = history.older();
        const double span = prev.serial != 0 && cur.tick > prev.tick ? cur.tick - prev.tick : 1.0;
        const double oldest = older.serial != 0  ? older.tick
                              : prev.serial != 0 ? prev.tick
                                                 : cur.tick;
        clock.published(cur.tick, oldest, span);
    }

    /// The two published ticks the drawn time falls between, and how far.
    osc::sim::FrameView view() const {
        const auto& older = history.older();
        const auto& prev = history.prev();
        const auto& cur = history.cur();
        const double at = clock.drawn();
        const auto between = [at](const osc::sim::WorldSnapshot& a,
                                  const osc::sim::WorldSnapshot& b) {
            const double alpha = (at - a.tick) / static_cast<double>(b.tick - a.tick);
            return osc::sim::FrameView(&a, &b, static_cast<float>(std::clamp(alpha, 0.0, 1.0)));
        };
        if (older.serial != 0 && prev.serial != 0 && at < prev.tick && prev.tick > older.tick)
            return between(older, prev);
        if (prev.serial != 0 && cur.tick > prev.tick) return between(prev, cur);
        return {&prev, &cur, 1.0f};
    }
};

// frames.cpp
bool mouse_over_ui(lua_State* uiL, f64 x, f64 y);
void cancel_command_mode(lua_State* uiL);
void call_command_graph(lua_State* uiL, const char* name, int nargs);
void sync_build_ghost(sim::SimState& sim, const renderer::CommandMode& m, bool& ghost_from_mode);

// ui_globals.cpp
/// Register the session's UI globals (FlushEvents, SessionIsReplay, ...) on
/// the UI state.
void register_session_ui_globals(lua::LuaState& ui_lua_state);
/// What the sim's SessionIsReplay() answers its scripts (the flow tests'
/// check that the sim and the UI agree).
bool sim_session_is_replay(const sim::SimState& sim);

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
    /// A UI state's own setup: bindings, the App's objects, userInit.lua --
    /// or, for a game's (`game_mods`: its mods, as sim::GameSetup::mods),
    /// SessionInit.lua.
    std::optional<int> init_ui_state(const std::string* game_mods = nullptr);
    /// start()'s part of it: the registries and managers the bindings use.
    void publish_session_objects();
    /// Replace the UI state with a fresh one, as Moho starts the front end
    /// and each game in a state of its own (M191 step 4). The old state's
    /// controls, threads, beat functions and key maps go with it; the
    /// caller then runs SetupUI and the front end's or game's UI. A game's
    /// state (`game_mods` given) is init_ui_state's for a game.
    void reset_ui_state(const std::string* game_mods = nullptr);
    /// The session, the UI's registries, and the test modes' front end.
    std::optional<int> start();

    // window.cpp. A value ends the run.
    std::optional<int> run_window();
    /// The windowed run: its loop's state and its frame's phases
    /// (window_loop.hpp, M192 step 2c).
    class Window;

    // headless.cpp
    int run_headless();
    /// One headless tick, timed for --bench.
    void tick_headless();
    /// After a headless tick: a loaded game (--load) is checked against its
    /// save until the player's turn, and --save-at's tick saves the game.
    /// False once a loaded game stops matching its save.
    bool after_headless_tick();
    /// After a tick of a loaded game catching up: check it against the save,
    /// and let catch_up go once the player has the game. False (logged) on
    /// the first tick that differs.
    bool check_catch_up();
    /// A loaded game is the player's: schedule its post-load (Moho's
    /// SyncPlayableRect and OnPostLoad), recorded in its history.
    void post_load();
    /// A loaded game's interface waits for the load's post-load, as Moho's
    /// runs at load, before the world's first sync and gamemain.CreateUI
    /// (which builds a campaign's objectives only in campaign mode). Set
    /// while it waits: the count of post-loads run when post_load scheduled
    /// the load's own, kPostLoadUnscheduled until then (a catch-up first
    /// replays the post-loads of earlier loads in the game's history).
    /// Window's finish_world_ui_after_post_load builds it.
    std::optional<u32> world_ui_after_post_load;
    static constexpr u32 kPostLoadUnscheduled = std::numeric_limits<u32>::max();
    /// A saved game restored from its snapshot into the sim just booted for
    /// it (M208c): its history becomes the recording, and its checksum must
    /// be the one the history holds for the saved tick. Only a snapshot this
    /// installation signed is restored. `why` says why not.
    enum class Restore {
        Done,
        Skipped, ///< no snapshot, or not this installation's: the sim is untouched
        Failed,  ///< the sim is spoiled: boot it again to catch up
    };
    Restore restore_save(const sim::SavedGame& save, std::string& why);

    /// This player's copy of a network game's save every player agreed to
    /// (OE-128), signed as their own, in their current profile's saves; the
    /// UI hears of it (OnNetworkGameSaved(name, worked), when it has one).
    void save_network_game(sim::SimState& sim, const std::string& name);
    /// Tell the UI of the network saves written since (between frames).
    void deliver_network_save_notices();
    /// The profile whose saves a network game's go in: the UI's when the
    /// game began (the sim's thread can't ask it).
    std::string network_profile;
    std::mutex network_saves_mutex;
    std::vector<std::pair<std::string, bool>> network_save_notices; ///< name, worked

    /// FA's LastGame: the game just left, as recorded, in the current
    /// profile's replays -- when a new game starts, on the way back to the
    /// lobby, and at exit. Interactive games only; a replay isn't recorded.
    void save_last_game();
    /// A player's game is kept: its recording, and its statistics as it
    /// plays (MatchTelemetry), under a name of its own beside LastGame --
    /// `<when it ended> <its map>.oscreplay` and `.jsonl` -- so that the
    /// game before last is still there to be watched again and studied.
    /// begin: opens the statistics of the game now starting, unless the run
    /// was given a file for them (--match-telemetry). Each frame may call it.
    void begin_match_record();
    /// Where the current profile keeps a recording called `base`; empty if
    /// its name can't be a folder's.
    std::filesystem::path recording_path(const std::string& base) const;
    /// Whether `telemetry` is a kept game's (begin_match_record's), to be
    /// closed and filed with its recording.
    bool match_record_open_ = false;

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
    /// The tick a loaded game was restored at (M208c), until the next launch.
    std::optional<u32> restored_at;
    bool save_written = false; ///< --save's file, once written
    /// --bench: each headless tick's time, from the run's start (M223).
    std::optional<BenchRecorder> bench;
    /// --match-telemetry: the armies' statistics as the match plays.
    std::optional<MatchTelemetry> telemetry;
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

    /// OpenURL's allowed protocols (the init file's) and the system browser;
    /// declared first, as the UI state keeps a pointer to it.
    lua::UrlOpener url_opener;
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
    ui::Console console; ///< Moho's console: key map actions and ConExecute run through it
    HeldConVars held_convars;
    renderer::RangeOverlays range_overlays;
    FrontEndData front_end_data;
    GameStateManager game_state_mgr;
    /// What a test mode drives.
    Engine engine;
    std::unique_ptr<lua::SmokeTestHarness> instrument_harness;
};

} // namespace osc::app
