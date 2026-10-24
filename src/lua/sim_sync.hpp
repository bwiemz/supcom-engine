#pragma once

#include "core/lua_copy.hpp"

#include <deque>
#include <optional>

struct lua_State;

namespace osc::lua {

/// Registry key (UI state, or the sim's) of a pending SetFocusArmy request:
/// 0-based army or -1 for observer. While a world session runs, the UI's
/// SetFocusArmy is a request the next sim beat applies, as Moho's
/// CWldSession does; so is the sim's (FAF's), the UI's winning a tie.
inline constexpr const char* kFocusArmyRequestKey = "__osc_focus_army_request";

/// The UI state's focus army: 0-based, -1 for an observer (0 when unset).
int focus_army(lua_State* ui_L);

/// Set both states' focus army outright, as a game starts (no
/// NoteFocusArmyChanged, no Sync.FocusArmyChanged; either state may be null).
void set_focus_army(lua_State* sim_L, lua_State* ui_L, int army);

/// Plain-data deep copy between states (see core/lua_copy.hpp).
using core::copy_lua_value;

/// The sim -> user channel's beats (Moho's mSyncDataQueue): each beat's Sync table, copied out
/// of the sim state as the beat ends, waits here for the user side; a save holds none of them.
class SyncQueue {
public:
    SyncQueue();
    ~SyncQueue();
    SyncQueue(const SyncQueue&) = delete;
    SyncQueue& operator=(const SyncQueue&) = delete;

    /// Moho's Sim::Sync, after each beat: the pending focus request applied, a copy of the
    /// sim's Sync queued, ResetSyncTable(). A null ui_L leaves the UI state alone.
    void queue(lua_State* sim_L, lua_State* ui_L);
    /// Between jobs, take the UI's focus request for the sim thread's next queue().
    void prepare_threaded(lua_State* sim_L, lua_State* ui_L);
    bool pending() const { return !packets_.empty(); }
    /// Moho's CWldSession::DoBeat: the oldest queued Sync becomes the user state's, then its
    /// OnSync(); with none queued, queue's steps run on the sim's Sync now.
    void deliver(lua_State* sim_L, lua_State* ui_L);
    void clear();

private:
    lua_State* packets_state_ = nullptr;
    std::deque<int> packets_;
    std::optional<int> ui_request_;
};

} // namespace osc::lua
