#pragma once

// --self-destruct-flow-test: a skirmish the player plays as ARMY_2, offscreen,
// launched from the front end as retail's lobby launches one (an AI in the
// first slot). The game's UI focuses the player's army; the player selects
// their commander and self-destructs it as Ctrl-K does (retail's
// ConfirmUnitDestruction, a SimCallback). The game runs on to kEndTick, or
// to its end. tests/integration/self_destruct_flow.py replays its recording
// headless, where nothing focuses ARMY_2: what the UI alone decides must not
// reach the sim's checksum.

#include "core/types.hpp"

#include <string>

namespace osc::lua {
class LuaState;
}
namespace osc::sim {
class SimState;
}

namespace osc::app {

class SelfDestructFlowTest {
public:
    static constexpr u32 kSelfDestructTick = 20;
    static constexpr u32 kEndTick = 200;

    /// With the front end up: launch the skirmish. False (reported) on a
    /// script error.
    bool start(lua::LuaState& ui);
    /// Each frame: the self-destruct at kSelfDestructTick, then the end.
    void frame(lua::LuaState& ui, const sim::SimState* sim);
    bool done() const { return done_; }
    /// The result, as test_status failures or a [PASS] line.
    void finish() const;

private:
    void fail(const std::string& why);

    u32 frames_ = 0;
    u32 acu_ = 0;
    u32 end_tick_ = 0;
    bool done_ = false;
};

} // namespace osc::app
