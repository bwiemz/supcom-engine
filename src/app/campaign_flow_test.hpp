#pragma once

// --campaign-flow-test (M209b): FA's campaign as a new player meets it,
// offscreen, through retail's own screens and buttons:
// - the main menu's Campaign, whose first visit plays the timeline movie,
//   which a click skips to the first operation's briefing;
// - Back to operation select, Select, then the briefing's Launch;
// - X1CA_001 loads, its interface in campaign mode and hidden for its
//   intro; its faction dialog takes a click on the UEF;
// - once it has given objectives, the game is saved as a campaign save
//   (InternalSaveGame, the CampaignSave type), listed, and loaded
//   (LoadSavedGame, as the Load dialog does); in the new UI state the
//   post-load has put campaign mode back and re-sent the objectives;
// - the operation is ended as its scripts end it on a win
//   (ScenarioFramework.EndOperation), its campaignInfo checked first;
// - the result dialog's Ok, the score screen's Continue;
// - the front end opens the next operation's briefing, X1CA_002, with
//   X1CA_001 finished and X1CA_002 unlocked in the profile.
// A script error fails it.

#include "core/types.hpp"

#include <string>

namespace osc::lua {
class LuaState;
}
namespace osc::sim {
class SimState;
}
namespace osc::ui {
class UIDispatch;
class UIControlRegistry;
} // namespace osc::ui

namespace osc::app {

class CampaignFlowTest {
public:
    /// Each frame: the next step, once its screen is up. `input` is where a
    /// player's clicks go.
    void frame(lua::LuaState& ui, lua::LuaState* sim_lua, const sim::SimState* sim,
               ui::UIDispatch& input, ui::UIControlRegistry& controls);
    bool done() const { return done_; }
    /// The result, as test_status failures or a [PASS] line.
    void finish() const;

private:
    enum class Step : u8 {
        Campaign,      ///< the main menu's button
        Timeline,      ///< the first visit's movie, skipped by a click
        FirstBriefing, ///< X1CA_001's briefing (from the movie): Back
        Select,        ///< operation select: Select
        Launch,        ///< the briefing: Launch
        Faction,       ///< in the game: the UEF
        Save,          ///< its objectives given: a campaign save, then its load
        Loaded,        ///< the loaded game: campaign mode and objectives back
        End,           ///< the faction taken: the operation won
        Ok,            ///< "Operation completed"
        Continue,      ///< the score screen
        NextBriefing,  ///< the front end: X1CA_002's briefing
        Done,
    };

    void fail(const std::string& why);
    u32 objectives_ = 0; ///< how many the UI held when the game was saved
    void next(Step step);

    Step step_ = Step::Campaign;
    u32 frames_ = 0;
    u32 step_frames_ = 0; ///< frames in this step
    bool done_ = false;
    bool passed_ = false;
};

} // namespace osc::app
