#pragma once

// --lan-game-host / --lan-game-join <address> (M218c): two processes play
// retail's LAN lobby (lobby.lua) to a game, offscreen. The host opens the
// lobby and hosts SCMP_009 on --mp-port; the other joins it once it
// listens. Each readies its slot, the host presses Launch, retail counts
// down and each launches, and the two play in lockstep over the lobby's
// connections. It passes when each reaches kCheckTick in step (no desync),
// playing its own slot's army with both armies human, and no script error.
// In the game (M218d) each sees the session's two clients and their
// sources; the host pauses (M218f) and every tick holds until the joiner
// resumes it, both hearing OnPause and OnResume; and its chat to everyone
// reaches both. The host's lobby makes the game's speed adjustable, and the
// joiner, resuming, raises it to +2 (M218i): both hear it, and play on at it.
//
// With `self_destruct_at` the joiner self-destructs its commander at that
// tick, as Ctrl-K does: retail's sim scripts fork threads for the focus army
// alone (a commander's under-attack voice-over), and each side focuses its
// own, yet the two must agree.
//
// With `quit_at` (M218e) the joiner leaves the game at that tick: the host
// must drop it by the lockstep's agreement, defeat its army, show and close
// retail's disconnect dialog without a script error, and play on alone.
//
// A network game saved and resumed (OE-128), in two runs on the same user
// folders. With `save_as` the host saves the game under that name at
// kSaveTick, through the in-game save (InternalSaveGame): each side must
// end with its own copy. With `resume` the host's lobby launches that save
// (GameOptions.ResumeSave, ResumeTick from its own copy): both must take up
// past tick 0 and play to kEndTick in step, without the first run's pause,
// chat and speed change. Both runs log their checksum at kEndTick, which
// must be the same (the players give no orders, so the resumed game must
// arrive where the saved one did). The host pings in the saved beat: the
// resumed game's interface must hear nothing of it.

#include "core/types.hpp"
#include "sim/entity_id.hpp"

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
    /// The tick the host saves on (save_as): before the check's pause.
    static constexpr u32 kSaveTick = 60;

    /// `address` and `port`: the host's lobby (the host listens on `port`).
    LanGameTest(bool host, std::string address, u16 port, u32 quit_at = 0,
                u32 self_destruct_at = 0);

    /// With the front end up: open retail's lobby (the host hosts at once;
    /// the other joins once the host listens). False (reported) on a script
    /// error.
    /// --lan-game-save <name>: the host saves the game as `name` at kSaveTick.
    void save_as(std::string name) { save_as_ = std::move(name); }
    /// --lan-game-resume <name>: the host's lobby launches that saved game.
    void resume(std::string name) { resume_ = std::move(name); }


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
    /// While the host's pause holds (M218f): no tick runs; the joiner
    /// resumes it after a while.
    void pause_frame(lua::LuaState& ui, const sim::SimState& sim);
    /// The UI heard the pause, from the host, and its end.
    void check_pause(lua::LuaState& ui);
    /// The joiner's speed change heard, and the game at it (M218i).
    void check_speed(lua::LuaState& ui);
    static bool joiner_defeated(const sim::SimState& sim);
    /// Whether retail's disconnect dialog is open (its file-local `parent`).
    bool disconnect_dialog_open(lua::LuaState& ui);
    void check_chat(lua::LuaState& ui);
    /// The checksum at kEndTick logged; this side's copy of a save, and a
    /// resumed game's start, checked.
    void check_save(lua::LuaState& ui, const sim::SimState& sim);
    void check_no_saved_sync(lua::LuaState& ui);
    void fail(std::string why);

    bool host_;
    std::string address_;
    u16 port_;
    u32 quit_at_ = 0; ///< the joiner leaves at this tick (0: it plays on)
    std::optional<u32> end_beat_; // replay checksum index for kEndTick
    u32 self_destruct_at_ = 0; ///< the joiner self-destructs then (0: never)
    sim::EntityId self_destructed_{}; ///< the joiner's commander, once it self-destructs
    bool self_destruct_failed_ = false;
    std::string save_as_;           ///< the host saves the game as this (none: no save)
    bool save_sent_ = false;        ///< it asked to
    bool pinged_ = false;           ///< the host's ping in the saved beat went
    std::string resume_;            ///< the host's lobby resumes this saved game (none: new)
    std::optional<u32> first_tick_; ///< the first tick seen in the game

    u32 frames_ = 0;
    bool joined_ = false;           ///< the joiner's JoinGame went out
    bool listening_ = false;        ///< the game's chat is heard
    bool left_ = false;             ///< the joiner left at quit_at_
    bool dialog_seen_ = false;      ///< the host saw retail's disconnect dialog open
    bool saw_pause_ = false;        ///< the host's pause held here
    std::optional<u32> paused_tick_; ///< the tick the pause holds on
    u32 paused_frames_ = 0;          ///< frames the joiner has seen it hold
    u32 last_press_ = 0;            ///< the frame Launch was last pressed
    std::optional<u32> checked_at_; ///< the frame the check ran
    std::optional<u32> over_at_;    ///< the frame the game reached its end
    bool done_ = false;
    std::string failure_;
};

} // namespace osc::app
