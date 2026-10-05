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
//
// --tutorial-flow-test takes the same screens to operation select, picks
// its Tutorial and launches it, then plays the tutorial's first missions as
// a player would: its opening camera move (no army has a unit yet: Moho
// spawns none for an operation), the zoom objective's camera marker brought
// on screen, the commander moved into its area once it has warped in, and
// the fodder it is sent at killed, until the build-mass mission starts.
//
// --outro-flow-test plays the campaign's end. A profile that has finished
// X1CA_001-005 as the UEF (and seen the timeline) picks X1CA_006 in
// operation select, launches it from its briefing, and wins it. After the
// result dialog the score screen plays retail's outro movies in turn -- the
// conclusion, the UEF's credits and the post-credits movie -- each skipped
// by a click, then shows the score; Continue returns to the main menu.
// Operation select then offers the credits: Play Movie, the UEF in the
// faction chooser, and the credits movie plays, back to operation select.

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
    /// Which way through: the campaign's first operation, the tutorial, or
    /// the campaign's end.
    enum class Route : u8 { Campaign, Tutorial, Outro };
    explicit CampaignFlowTest(Route route = Route::Campaign)
        : route_(route), step_(route == Route::Outro ? Step::OutroSeed : Step::Campaign) {}
    /// Each frame: the next step, once its screen is up. `input` is where a
    /// player's clicks go.
    void frame(lua::LuaState& ui, lua::LuaState* sim_lua, const sim::SimState* sim,
               ui::UIDispatch& input, ui::UIControlRegistry& controls);
    bool done() const { return done_; }
    /// The result, as test_status failures or a [PASS] line.
    void finish() const;

private:
    enum class Step : u8 {
        Campaign,          ///< the main menu's button
        Timeline,          ///< the first visit's movie, skipped by a click
        FirstBriefing,     ///< X1CA_001's briefing (from the movie): Back
        Select,            ///< operation select: Select
        Launch,            ///< the briefing: Launch
        Faction,           ///< in the game: the UEF
        Save,              ///< its objectives given: a campaign save, then its load
        Loaded,            ///< the loaded game: campaign mode and objectives back
        End,               ///< the faction taken: the operation won
        Ok,                ///< "Operation completed"
        Continue,          ///< the score screen
        NextBriefing,      ///< the front end: X1CA_002's briefing
        TutorialEntry,     ///< operation select: its Tutorial entry
        TutorialLaunch,    ///< its Launch Tutorial
        TutorialOpening,   ///< the opening camera move done: the zoom objective
        TutorialZoom,      ///< its camera marker on screen
        TutorialMove,      ///< the commander warped in: moved into its area
        TutorialAttack,    ///< the fodder killed, the build-mass mission begun
        OutroSeed,         ///< the profile's progress to X1CA_006; Campaign
        OutroPick,         ///< operation select: X1CA_006, Select
        OutroBriefing,     ///< X1CA_006's briefing: Launch
        OutroEnd,          ///< in the game: X1CA_006 won
        OutroOk,           ///< "Operation completed"
        OutroMovies,       ///< the outro, credits and post-credits movies; Continue
        OutroMenu,         ///< the main menu: Campaign
        OutroCredits,      ///< operation select: Credits, Play Movie
        OutroFaction,      ///< the faction chooser: the UEF
        OutroCreditsMovie, ///< its credits played, back to operation select
        Done,
    };

    void fail(const std::string& why);
    u32 objectives_ = 0; ///< how many the UI held when the game was saved
    void next(Step step);

    Route route_ = Route::Campaign;
    Step step_ = Step::Campaign;
    u32 frames_ = 0;
    u32 step_frames_ = 0; ///< frames in this step
    bool done_ = false;
    bool passed_ = false;
    bool ordered_ = false; ///< this step's camera move or order is given
    u32 movies_seen_ = 0;  ///< the outro's movies seen playing, one bit each
};

} // namespace osc::app
