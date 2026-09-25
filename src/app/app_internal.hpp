#pragma once

// What the application's own files share (M192 step 2): app.cpp's run() and
// the files split from it. Not for the tests, which use support.hpp.

#include "app/support.hpp"
#include "core/tick_clock.hpp"
#include "lua/init_loader.hpp"
#include "sim/replay.hpp"
#include "sim/sim_state.hpp"
#include "sim/world_snapshot.hpp"

#include <fstream>
#include <optional>
#include <string>

namespace osc::audio {
class SoundManager;
}
namespace osc::lua {
class LuaState;
}
namespace osc::sim {
class SimRandom;
}

namespace osc::app {

// cli.cpp
void print_usage();
lua::InitConfig parse_args(int argc, char* argv[], const TestModes* tests);
u32 parse_ticks_arg(int argc, char* argv[]);
std::string parse_map_arg(int argc, char* argv[]);
/// The test mode flag on the command line, if any.
const char* test_mode_flag(int argc, char* argv[]);

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

} // namespace osc::app
