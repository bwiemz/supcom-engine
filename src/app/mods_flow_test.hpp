#pragma once

// --mods-flow-test (M221b, M221c): a skirmish with mods, offscreen. It is
// launched one of three ways:
// - Direct: from the front end the way retail's lobby launches one -- its
//   GameMods is Mods.GetGameMods(), the mods the player selected
//   (preference active_mods) that aren't UI-only;
// - Lobby (--mods-flow-lobby): through retail's own front end and skirmish
//   lobby, as a player clicks them: Skirmish (no tutorial), Game Options,
//   Mods, Resource Rich in the mod manager, Enter, OK, Launch (with an AI
//   added, as a skirmish needs two players);
// - Watch (--watch <replay>): a recorded game, with its own mods;
// - Command line (--map, with --mod): the game the command line starts,
//   with the mods --mod picks in place of the player's selection.
// Once the game has run to kReportTick it reports, for the sim's Lua state
// and the game UI's, what their mods made of them: __active_mods (uids, in
// order), the UEF ACU's blueprint, the probes test mods' hooks set, and the
// player's selected mods (preference active_mods). And one emitter
// blueprint a test mod merges into, as the engine reads it: the Lifetime
// the sim times its emitters by, and the Lifetime and Texture the renderer
// draws them with.
// tests/integration/mods_flow.py sets up the mods and reads the report. A
// script error fails it.

#include "core/types.hpp"

#include <optional>
#include <string>

struct lua_State;

namespace osc::lua {
class LuaState;
}
namespace osc::renderer {
class Renderer;
}
namespace osc::sim {
class SimState;
}
namespace osc::ui {
class UIControl;
class UIDispatch;
class UIControlRegistry;
} // namespace osc::ui

namespace osc::app {

class ModsFlowTest {
public:
    enum class Launch : u8 { Direct, Lobby, Watch, CommandLine };

    static constexpr u32 kReportTick = 20;

    explicit ModsFlowTest(Launch launch) : launch_(launch) {}

    /// With the front end up: launch the skirmish, or open the lobby. False
    /// (reported) on a script error.
    bool start(lua::LuaState& ui);
    /// Each frame: in the lobby, its next step; in the game, the report at
    /// kReportTick. `input` is where a player's clicks go; `renderer` draws
    /// the game, with the UI state's blueprints.
    void frame(lua::LuaState& ui, lua::LuaState* sim_lua, const sim::SimState* sim,
               ui::UIDispatch& input, ui::UIControlRegistry& controls,
               renderer::Renderer& renderer);
    bool done() const { return done_; }
    /// The result, as test_status failures or a [PASS] line.
    void finish() const;

private:
    /// The lobby's steps, each taken once its control is shown.
    enum class Step : u8 {
        Skirmish,
        Tutorial,
        AddAi,
        Options,
        ModsButton,
        Mod,
        Close,
        MapOk,
        Launch,
        Launched,
    };

    void lobby_frame(lua::LuaState& ui, ui::UIDispatch& input, ui::UIControlRegistry& controls);
    /// The report line for one Lua state, or why it has none.
    static std::string report(lua_State* L);
    /// The report line for kEffect: as the sim times its emitters, and as
    /// `renderer` draws them.
    static std::string effect_report(lua_State* sim_L, const sim::SimState& sim, lua_State* ui_L,
                                     renderer::Renderer& renderer);
    void fail(const std::string& why);

    Launch launch_;
    Step step_ = Step::Skirmish;
    u32 frames_ = 0;
    u32 step_frames_ = 0; ///< frames in this step
    bool reported_ = false;
    bool done_ = false;
};

/// A control a test found, and its centre.
struct FoundControl {
    const ui::UIControl* control = nullptr;
    f64 x = 0;
    f64 y = 0;
};

/// The newest shown control that `match` -- the body of a Lua function of
/// the control `c`, returning true for it -- picks. For tests that click
/// through retail's dialogs.
std::optional<FoundControl> find_control(lua_State* L, ui::UIControlRegistry& controls,
                                         const std::string& match);

/// A find_control match for a button whose label reads what `loc` localizes to.
std::string labelled(const char* loc);

/// The control a click at (x, y) would reach, as the dispatch hit-tests:
/// from the top input capture, else the root frame (null: none).
const ui::UIControl* control_at(lua_State* L, ui::UIDispatch& input,
                                ui::UIControlRegistry& controls, f64 x, f64 y);

/// A player's left click on `found`, once it takes one: the mouse there,
/// pressed and let go. False if it doesn't take one yet.
bool click(lua_State* L, ui::UIDispatch& input, ui::UIControlRegistry& controls,
           const std::optional<FoundControl>& found);

} // namespace osc::app
