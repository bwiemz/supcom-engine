#pragma once

// --restart-flow-test: a skirmish launched from the front end as retail's
// lobby launches one, offscreen. At kRestartTick the player restarts it as
// retail's game menu does (its Restart Game confirmation's Yes calls
// RestartSession()). The game must start again from its launch, as Moho's
// CreateSession frame action starts it: a new sim, from tick 0, with the
// scenario, armies and seed it was launched with. It plays on to kEndTick.

#include "core/types.hpp"

#include <string>

namespace osc::lua {
class LuaState;
}
namespace osc::sim {
class SimState;
}

namespace osc::app {

class RestartFlowTest {
public:
    static constexpr u32 kRestartTick = 30;
    static constexpr u32 kEndTick = 60;

    /// With the front end up: launch the skirmish. False (reported) on a
    /// script error.
    bool start(lua::LuaState& ui);
    /// Each frame: the restart at kRestartTick, then the new game to its end.
    void frame(lua::LuaState& ui, const sim::SimState* sim);
    bool done() const { return done_; }
    /// The result, as test_status failures or a [PASS] line.
    void finish() const;

private:
    void fail(const std::string& why);

    u32 frames_ = 0;
    /// The first game, as RestartSession was called in it.
    u32 first_game_ = 0; ///< its sim's generation
    u64 first_seed_ = 0;
    std::string first_scenario_;
    size_t first_armies_ = 0;
    bool restarted_ = false;
    u32 frames_since_restart_ = 0;
    bool restart_seen_ = false;
    u32 end_tick_ = 0;
    bool done_ = false;
};

} // namespace osc::app
