#pragma once

// The windowed run (M192 step 2c): what App::run_window's loop keeps, and
// its frame broken into the phases it runs in. It reaches the App's state
// through references named as the App names it, so each phase reads as the
// loop did; its own state is the loop's locals, declared in the order the
// loop made them, so they are destroyed in the reverse of it.

#include "app/app_internal.hpp"
#include "app/lan_game_test.hpp"
#include "app/mods_flow_test.hpp"
#include "app/window_mode.hpp"
#include "lua/factory_queue.hpp"
#include "renderer/input_handler.hpp"
#include "renderer/renderer.hpp"
#include "sim/sim_callback_queue.hpp"

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace osc::app {

class App::Window {
public:
    explicit Window(App& app);

    /// The window, its frames until one ends the run, and what the run's
    /// checks say. A value ends the run.
    std::optional<int> run();

private:
    // window.cpp: before and after the frames. A value ends the run.
    /// The window, the scene, input and the captures' setup.
    std::optional<int> set_up();
    /// The window's objects in the UI state's registry, again for each new
    /// UI state (M191 step 4).
    void publish_window_objects();
    /// --watch, --replay-flow-test: the replay, through retail's globals.
    std::optional<int> open_replay();
    /// --load, --load-flow-test: the saved game, through retail's globals.
    std::optional<int> open_saved_game();
    /// --lan-game-host/-join and --mods-flow-test: their flows' start.
    std::optional<int> start_flows();
    /// Whether the frames go on: the window open, no capture or scripted
    /// flow done.
    bool running() const;
    /// The scripted flows' and captures' verdicts, once the frames end.
    std::optional<int> finish();
    /// Vulkan failed: a capture fails, anything else runs headless.
    std::optional<int> run_without_renderer();

    // window_frame.cpp: one frame, in its phases.
    /// One frame. False when it ends the run (the UI asked to exit).
    bool frame();
    /// The profiler's frame, a resized root frame, and this frame's time.
    double begin_frame();
    /// The camera as the sound's listener.
    void update_audio(double dt);
    /// The world's sounds for this frame: the ticks' one-shots and the
    /// entities' loops, through Moho's filter (M216b).
    void update_world_sounds();
    void count_fps(double dt);
    /// The sim's ticks for this frame: a loaded game catching up, a
    /// lockstep game's rounds, or the local clock's.
    void advance_sim(double dt);
    /// The sim's beat to the user side, once a tick (paused too).
    void run_beats(double dt, u32 beat_tick0);
    /// The scripted flows' frame: replay, LAN, GPGNet, mods, load.
    void run_flows();
    /// The UI's frame: the camera's input, events, the networks, the UI's
    /// threads and beats.
    void update_ui(double dt);
    /// Clicks and keys on this frame's world: the focus army, selection
    /// and orders.
    void update_input(double dt, const sim::FrameView& frame_view);
    /// This frame drawn, and captured when it is the capture's.
    void render(const sim::FrameView& frame_view);
    void update_title(double dt);

    // window_session.cpp: the UI's requests between games.
    /// The UI asked to exit (ExitApplication).
    bool exit_requested();
    /// A game, replay or saved game launched from the lobby or a dialog.
    void handle_launch();
    /// Back to the front end (the score screen's Continue, ReturnToLobby).
    void handle_return_to_lobby();

    // window_load_flow.cpp: --load-flow-test.
    /// Its globals, into the current UI state.
    void load_flow_globals();
    /// Its frame: each load catches up, plays on, and moves to the next.
    void load_flow_frame();
    /// Once the loaded game has played on: save it again as retail's Save
    /// dialog does, and check what saving refuses.
    void save_again_in_load_flow();
    /// The loads' resume ticks against the plan.
    int finish_load_flow();

    // The App's own work, under the names the loop calls it by.
    void save_last_game() { app.save_last_game(); }
    void reset_ui_state(const std::string* game_mods = nullptr) { app.reset_ui_state(game_mods); }
    bool check_catch_up() { return app.check_catch_up(); }
    Restore restore_save(const sim::SavedGame& save, std::string& why) {
        return app.restore_save(save, why);
    }

    static constexpr double kScreenshotFrameDt = 1.0 / 60.0;
    /// Scripted windowed runs (a test mode's, --replay-flow-test): four
    /// frames per sim tick, on a fixed clock.
    static constexpr double kInterpFrameDt = sim::SimState::SECONDS_PER_TICK / 4.0;
    /// A loaded game catching up ticks for this long each frame.
    static constexpr auto kCatchUpFrameBudget = std::chrono::milliseconds(100);

    App& app;
    // The App's state, as the loop named it.
    int& argc = app.argc;
    char**& argv = app.argv;
    TestModes*& tests = app.tests;
    const lua::InitConfig& config = app.config;
    Options& opt = app.opt;
    std::unique_ptr<lua::LuaState>& sim_lua_state = app.sim_lua_state;
    vfs::VirtualFileSystem& vfs = app.vfs;
    lua::InitLoader& loader = app.loader;
    blueprints::BlueprintStore& store = app.store;
    audio::SoundManager& sound = app.sound;
    WorldInterp& world_interp = app.world_interp;
    std::unique_ptr<sim::SimState>& sim_state = app.sim_state;
    lua::ScenarioMetadata& scenario_meta = app.scenario_meta;
    std::optional<sim::ReplayPlayback>& catch_up = app.catch_up;
    std::optional<u32>& restored_at = app.restored_at;
    lua::LuaState& ui_lua_state = app.ui_lua_state;
    blueprints::BlueprintStore& ui_store = app.ui_store;
    core::Preferences& prefs = app.prefs;
    std::optional<lua::SpecialFiles>& special_files = app.special_files;
    ui::WldUIProvider& wld_provider = app.wld_provider;
    ui::UIControlRegistry& ui_registry = app.ui_registry;
    sim::ThreadManager& ui_thread_manager = app.ui_thread_manager;
    u32& ui_frame_count = app.ui_frame_count;
    lua::BeatFunctionRegistry& beat_registry = app.beat_registry;
    ui::Console& console = app.console;
    GameStateManager& game_state_mgr = app.game_state_mgr;
    Engine& engine = app.engine;
    std::unique_ptr<lua::SmokeTestHarness>& instrument_harness = app.instrument_harness;

    // The loop's own state, in the order it made it.
    osc::renderer::Renderer renderer;
    /// Scripted captures and checks render offscreen: no window to show,
    /// focus to steal, or compositor to wait for.
    const bool offscreen_capture;
    /// A player's window opens as FA's command line and options say
    /// (M217h); captures and scripted checks keep their fixed 1600x900.
    bool adapter_overridden = false;
    /// The UI's root frame: the size the window was asked for, until its
    /// swapchain has it (the resize lands a frame or more later), then the
    /// swapchain's (review: the boot laid the front end out at 1600x900).
    u32 root_width = 0;
    u32 root_height = 0;
    std::vector<Resolution> display_modes;
    /// Player input handler (ARMY_1 = index 0)
    osc::renderer::InputHandler input_handler;
    /// Factory queue display (M140c)
    osc::lua::FactoryQueueDisplay factory_queue;
    /// SimCallback queue (UI→Sim bridge, M138a)
    osc::sim::SimCallbackQueue sim_callback_queue;
    double sim_accumulator = 0.0;
    std::optional<osc::sim::ReplayPlayback> active_playback; // a replay being watched
    double paused_beat_accumulator = 0.0;
    /// FA's command mode drives world clicks (read once per frame).
    osc::renderer::CommandMode current_command_mode;
    bool ghost_from_mode = false;
    std::chrono::high_resolution_clock::time_point prev_time;
    double title_update_timer = 0.0;
    double fps_accum = 0.0;
    int fps_frames = 0;
    double display_fps = 0.0;
    std::unordered_set<osc::u32> prev_selection;
    /// --screenshot <png> [--screenshot-frame N]: render N frames on a
    /// fixed 60 Hz clock (so frame N is identical run to run), capture the
    /// presented image, write it, and exit. Used for golden-image tests and
    /// documentation shots. --golden <name> [--golden-update]: capture
    /// like --screenshot and compare against <golden dir>/<name>.png
    /// (OSC_GOLDEN_DIR, else the user state dir). Goldens contain game art,
    /// so they live outside the repository; a missing golden exits 77
    /// (CTest "skipped").
    std::string golden_name;
    bool golden_update = false;
    std::filesystem::path golden_path;
    std::string screenshot_path;
    osc::u32 screenshot_frame = 0;
    osc::u32 frames_rendered = 0;
    bool screenshot_done = false;
    bool screenshot_ok = false;
    bool replay_flow_done = false;
    osc::u32 replay_flow_frames = 0;
    // --load-flow-test, as a player might go (M208a, M191 step 4): see
    // window_load_flow.cpp. Each game starts in a UI state of its own, so
    // what the flow needs (the save's profile and file) is kept here, not
    // in Lua globals.
    bool load_flow_done = false;
    int load_flow_phase = 0;
    bool load_flow_saw_catch_up = false;          // this load began catching up
    std::optional<osc::u32> load_flow_resumed_at; // ...and was the player's from here
    std::vector<osc::u32> load_flow_resumes;      // every load's resume tick
    osc::u32 load_flow_frames = 0;
    osc::u32 load_flow_front_end_frames = 0;
    std::string load_flow_profile;
    std::string load_flow_file;
    /// --lan-game-host / --lan-game-join: retail's LAN lobby, from here to
    /// a game in lockstep with the other process (M218c).
    std::optional<osc::app::LanGameTest> lan_game;
    /// --mods-flow-test: a skirmish with mods (M221b), launched from the
    /// front end, through retail's lobby and its mod manager (M221c), or a
    /// recorded one watched.
    std::optional<osc::app::ModsFlowTest> mods_flow;
    /// The matchmaking client closed its link while no game plays: without
    /// it the game has nothing to do (M220a).
    bool gpgnet_done = false;
    osc::u32 gpgnet_logged_tick = 0;
    /// OnFirstUpdate, once after the game's first tick (each game's).
    bool first_update_fired = false;
};

} // namespace osc::app
