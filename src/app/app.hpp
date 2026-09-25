#pragma once

// The engine as an application (M192): the boot, the front end, a game's
// session, the windowed loop and the headless run. opensupcom runs it as it
// is; the integration runner (tests/integration/runner) adds its test modes
// through TestModes.

#include "core/types.hpp"

#include <memory>
#include <optional>
#include <string>

namespace osc {
class GameStateManager;
}
namespace osc::audio {
class SoundManager;
}
namespace osc::blueprints {
class BlueprintStore;
}
namespace osc::lua {
class BeatFunctionRegistry;
class InitLoader;
class LuaState;
struct InitConfig;
struct ScenarioMetadata;
} // namespace osc::lua
namespace osc::renderer {
class InputHandler;
class Renderer;
} // namespace osc::renderer
namespace osc::sim {
class SimState;
class ThreadManager;
class FrameView;
} // namespace osc::sim
namespace osc::ui {
class UIControlRegistry;
class WldUIProvider;
} // namespace osc::ui
namespace osc::vfs {
class VirtualFileSystem;
}

namespace osc::app {

/// The engine run has built, for a test mode to drive: references into the
/// run, valid for the call they are passed to. The sim and its Lua state
/// are owned here, so a mode may replace them (a reload) or drop them.
struct Engine {
    const lua::InitConfig& config;
    const std::string& map_path;
    u32 tick_count;              ///< --ticks (0: not given)
    const std::string& seed_arg; ///< --seed
    const std::string& ai_personality;
    vfs::VirtualFileSystem& vfs;
    blueprints::BlueprintStore& store;
    lua::InitLoader& loader;
    std::unique_ptr<lua::LuaState>& sim_lua_state;
    std::unique_ptr<sim::SimState>& sim_state;
    lua::ScenarioMetadata& scenario_meta;
    lua::LuaState& ui_lua_state;
    ui::UIControlRegistry& ui_registry;
    sim::ThreadManager& ui_thread_manager;
    u32& ui_frame_count;
    lua::BeatFunctionRegistry& beat_registry;
    GameStateManager& game_state_mgr;
    ui::WldUIProvider& wld_provider;
    audio::SoundManager& sound;
};

/// A frame of the windowed loop, as a test sees it.
struct Frame {
    const sim::FrameView& view;
    renderer::Renderer& renderer;
    renderer::InputHandler& input;
};

/// What the command line asks of the run, as far as the test modes go.
struct TestRequest {
    /// A headless test mode: the run is headless and repeatable, and its
    /// exit code is the checks' result.
    bool headless = false;
    /// A scripted windowed mode (it checks what is drawn): offscreen,
    /// silent, on a fixed clock.
    bool windowed = false;
    /// ARMY_2 plays as an AI.
    bool ai_army_2 = false;
    /// Headless, but with FA's game interface built.
    bool world_ui = false;
};

/// The test modes an integration runner adds to the run. The hooks sit
/// where the modes ran when they were part of main(); opensupcom has none.
class TestModes {
public:
    virtual ~TestModes() = default;
    /// The modes' part of --help.
    virtual void print_usage() const = 0;
    /// Which modes the command line asks for. Called once, first.
    virtual TestRequest parse(int argc, char* argv[]) = 0;
    /// Before any engine init: modes that need none. A value ends the run.
    virtual std::optional<int> before_boot(int argc, char* argv[]) = 0;
    /// Once FA is found, before the engine's init: modes that only read its
    /// files. A value ends the run.
    virtual std::optional<int> before_init(const lua::InitConfig& config) = 0;
    /// The front end booted (no map), before the loop. A value ends the run.
    virtual std::optional<int> front_end(Engine& engine) = 0;
    /// Windowed: each frame, with what it draws (before input and render).
    virtual void frame_view(Engine& engine, Frame& frame) = 0;
    /// Windowed: each frame, after it is rendered.
    virtual void frame_rendered(Engine& engine, Frame& frame) = 0;
    /// Windowed: whether the modes have seen enough frames.
    virtual bool frames_done() const = 0;
    /// Windowed: the window closed. A value ends the run.
    virtual std::optional<int> after_window() = 0;
    /// Headless, the game booted, before the run's own headless modes
    /// (--ai-skirmish, --ticks). A value ends the run.
    virtual std::optional<int> headless_first(Engine& engine) = 0;
    /// Headless, after the run's own headless modes.
    virtual void headless(Engine& engine) = 0;
};

/// Run the engine for this command line. `tests` may be null (the game).
int run(int argc, char* argv[], TestModes* tests);

} // namespace osc::app
