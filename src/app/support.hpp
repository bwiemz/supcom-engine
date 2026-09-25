#pragma once

// What the game loop and the integration runner's test modes both drive
// (app.cpp): the command line, a test run's result, UI frames and the sim
// beat, FA's world UI, selection and command-mode dispatch, and the reload
// into a new game.

#include "app/app.hpp"
#include "renderer/input_handler.hpp"

#include <string>
#include <unordered_set>

struct lua_State;

namespace osc::sim {
class SimCallbackQueue;
struct Replay;
} // namespace osc::sim

namespace osc::app {

struct WorldInterp;

/// Exit code for a test mode run when no game data is available: CTest
/// treats it as "skipped" (SKIP_RETURN_CODE), so data-backed tests pass
/// harmlessly on machines without Forged Alliance.
inline constexpr int kExitSkippedNoData = 77;

bool parse_flag(int argc, char* argv[], const char* flag);
std::string parse_string_arg(int argc, char* argv[], const char* flag,
                             const char* default_val = "");

/// End a test-mode run: fold smoke-harness issues into the tally, print a
/// summary, and return the process exit code (0 = all checks passed).
int finish_test_run(const char* mode, u32 smoke_issues = 0);

/// The UI state reaches the sim through a registry pointer; clear it before
/// the sim is destroyed so UI scripts see "no sim" rather than freed memory.
void detach_ui_from_sim(lua_State* uiL);

/// `count` UI frames: the UI clock, its threads, OnBeat and beat functions.
void pump_ui_frames(lua::LuaState& ui_lua_state, sim::ThreadManager& ui_thread_manager,
                    lua::BeatFunctionRegistry& beat_registry, int count, u32& ui_frame_counter);
/// pump_ui_frames, and the controls' frame update and event dispatch.
void pump_ui_frames_with_controls(lua::LuaState& ui_lua_state,
                                  sim::ThreadManager& ui_thread_manager,
                                  lua::BeatFunctionRegistry& beat_registry,
                                  ui::UIControlRegistry& ui_registry, int count,
                                  u32& ui_frame_counter);

/// One Moho sim beat reaching the user side.
void world_beat(lua::LuaState* sim_lua, sim::SimState* sim, lua_State* uiL);
/// The game has ended: tell the UI once (NoteGameOver).
void note_game_over_if_ended(sim::SimState* sim, GameStateManager& mgr, lua_State* uiL);
/// FA's game interface: the loading dialog, then the interface itself.
void begin_world_ui(lua_State* uiL, ui::WldUIProvider& wld);
void finish_world_ui(lua_State* uiL, ui::WldUIProvider& wld, bool is_replay);

/// The UI's OnSelectionChanged, when the selection changed.
void dispatch_selection_change(lua_State* uL, std::unordered_set<u32>& prev,
                               const std::unordered_set<u32>& cur, bool action);
/// FA's command mode (commandmode.lua), as world clicks read it.
renderer::CommandMode read_command_mode(lua_State* uiL);
/// A command a world click issued, told to the UI (OnCommandIssued).
void report_command_issued(lua_State* uiL, const renderer::IssuedCommand& c);
/// The UI's SimCallbacks, into the sim.
void submit_sim_callbacks(sim::SimCallbackQueue& queue, sim::SimState& sim);

/// The random seed of a new game: `--seed` when given; else a fixed one when
/// the run must repeat (tests, headless runs, captures); else a fresh one.
u64 new_game_seed(const std::string& seed_arg, bool reproducible);

/// Replace the game with a new one of `launch_scenario` (or `replay`): a
/// fresh sim Lua state and sim, its scenario, its boot and its session, and
/// the renderer's scene. The pointers may be null (headless). False on a
/// critical failure.
bool execute_reload_sequence(std::unique_ptr<lua::LuaState>& sim_lua_state,
                             std::unique_ptr<sim::SimState>& sim_state, lua::LuaState& ui_lua_state,
                             vfs::VirtualFileSystem& vfs, blueprints::BlueprintStore& store,
                             lua::InitLoader& loader, const lua::InitConfig& config,
                             lua::ScenarioMetadata& scenario_meta, GameStateManager& game_state_mgr,
                             renderer::Renderer* renderer, renderer::InputHandler* input_handler,
                             std::unordered_set<u32>* prev_selection, WorldInterp* world_interp,
                             u64 seed, double& sim_accumulator, const std::string& launch_scenario,
                             const sim::Replay* replay = nullptr);

} // namespace osc::app
