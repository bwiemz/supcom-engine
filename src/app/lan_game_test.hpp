#pragma once

// --lan-game-host / --lan-game-join <address> (M218c): two processes play
// retail's LAN lobby (lobby.lua) to a game, offscreen. The host opens the
// lobby and hosts SCMP_009 on --mp-port; the other joins it once it
// listens. Each readies its slot, the host presses Launch, retail counts
// down and each launches, and the two play in lockstep over the lobby's
// connections. It passes when each reaches kCheckTick in step (no desync),
// playing its own slot's army with both armies human, and no script error.
// In the game (M218d) each sees the session's two clients and their
// sources; the host's pause is refused (the game plays on), and its chat
// to everyone reaches both.

#include "core/types.hpp"

#include <optional>
#include <string>

namespace osc::lua {
class LuaState;
}
namespace osc::sim {
class SimState;
}

namespace osc::app {

class LanGameTest {
public:
    /// The tick both sides check at, and (a little later, so neither stops
    /// before the other has had its last frame) the one they stop at.
    static constexpr u32 kCheckTick = 100;
    static constexpr u32 kEndTick = 150;

    /// `address` and `port`: the host's lobby (the host listens on `port`).
    LanGameTest(bool host, std::string address, u16 port);

    /// With the front end up: open retail's lobby (the host hosts at once;
    /// the other joins once the host listens). False (reported) on a script
    /// error.
    bool start(lua::LuaState& ui);
    /// Each frame: in the lobby, ready this player's slot and (the host)
    /// press Launch once everyone is ready; in the game, check at kCheckTick.
    void frame(lua::LuaState& ui, const sim::SimState* sim);
    bool done() const { return done_; }
    /// The result, as test_status failures or a [PASS] line.
    void finish(const sim::SimState* sim) const;

private:
    void lobby_frame(lua::LuaState& ui);
    void check(lua::LuaState& ui, const sim::SimState& sim);
    /// The game's chat heard (gamemain's RegisterChatFunc), and checked.
    void listen_for_chat(lua::LuaState& ui);
    void check_chat(lua::LuaState& ui);
    void fail(std::string why);

    bool host_;
    std::string address_;
    u16 port_;
    u32 frames_ = 0;
    bool joined_ = false;           ///< the joiner's JoinGame went out
    bool listening_ = false;        ///< the game's chat is heard
    u32 last_press_ = 0;            ///< the frame Launch was last pressed
    std::optional<u32> checked_at_; ///< the frame the check ran
    bool done_ = false;
    std::string failure_;
};

} // namespace osc::app
