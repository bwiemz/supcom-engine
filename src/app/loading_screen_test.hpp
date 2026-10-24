#pragma once

// --loading-screen-test: the loading screen plays while a game loads, as
// Moho's frames go on through the load (WLD_DoLoading returns at once each
// frame until its loader's thread is done). Offscreen: a skirmish launched
// from the front end as retail's lobby launches one, then, once it has
// played a while, saved and loaded as the Load dialog does. Each load must
// draw frames with its loading movie (the faction's, retail's LoadDialog
// plays looped) while its world loads, and the saved game's while its
// snapshot restores. (The engine had loaded a game in one call between one
// frame and StopLoadingDialog, and a movie shows from its second frame.)

#include "core/types.hpp"

#include <array>
#include <optional>
#include <string>

namespace osc::lua {
class LuaState;
}
namespace osc::sim {
class SimState;
}
namespace osc::ui {
class UIControlRegistry;
}

namespace osc::app {

class LoadingScreenTest {
public:
    /// The skirmish's tick to be saved at, and how long its load plays on.
    static constexpr u32 kSaveTick = 30;
    static constexpr u32 kPlayOn = 10;

    /// With the front end up: launch the skirmish. False (reported) on a
    /// script error.
    bool start(lua::LuaState& ui);
    /// A frame drawn while a game loads: whether the loading movie's frame
    /// was drawn in it; `restoring` while a saved game's snapshot restores.
    void loading_frame_drawn(const ui::UIControlRegistry& registry, bool restoring);
    /// Each of the game's frames, with its interface up (a loaded game's
    /// waits for its post-load): the skirmish saved and loaded at
    /// kSaveTick, then the end once the load has played on.
    void frame(lua::LuaState& ui, const sim::SimState* sim, bool interface_up,
               std::optional<u32> restored_at);
    bool done() const { return done_; }
    /// The result, as test_status failures or a [PASS] line.
    void finish() const;

private:
    void fail(const std::string& why);

    /// Each load's frames: drawn while it loaded, those with the movie
    /// drawn, and of those a saved game's while its snapshot restored.
    struct Load {
        u32 frames = 0;
        u32 movie_frames = 0;
        u32 restore_frames = 0;
        u32 restore_movie_frames = 0;
    };
    std::array<Load, 2> loads_{};
    int phase_ = 0; ///< 0: the skirmish, 1: its saved game
    u32 frames_ = 0;
    std::optional<u32> restored_at_;
    bool done_ = false;
};

} // namespace osc::app
