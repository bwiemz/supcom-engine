#pragma once

// --mods-flow-test (M221b): a skirmish with the player's mods, launched from
// the front end the way retail's lobby launches one -- its GameMods is
// Mods.GetGameMods(), the mods the player selected (preference active_mods)
// that aren't UI-only -- or, with --watch, a recorded game with its own
// mods. Offscreen. Once the game has run to kReportTick it
// reports, for the sim's Lua state and the game UI's, what their mods made
// of them: __active_mods (uids, in order), the UEF ACU's blueprint, and the
// probes test mods' hooks set. tests/integration/mods_flow.py sets up the
// mods and reads the report. A script error fails it.

#include "core/types.hpp"

#include <string>

struct lua_State;

namespace osc::lua {
class LuaState;
}
namespace osc::sim {
class SimState;
}

namespace osc::app {

class ModsFlowTest {
public:
    static constexpr u32 kReportTick = 20;

    /// With the front end up: launch the skirmish -- unless `launch` is
    /// false, when a replay the run watches (--watch) is the game. False
    /// (reported) on a script error.
    bool start(lua::LuaState& ui, bool launch);
    /// Each frame: once the game reaches kReportTick, report.
    void frame(lua::LuaState& ui, lua::LuaState* sim_lua, const sim::SimState* sim);
    bool done() const { return done_; }
    /// The result, as test_status failures or a [PASS] line.
    void finish() const;

private:
    /// The report line for one Lua state, or why it has none.
    static std::string report(lua_State* L);

    u32 frames_ = 0;
    bool reported_ = false;
    bool done_ = false;
};

} // namespace osc::app
