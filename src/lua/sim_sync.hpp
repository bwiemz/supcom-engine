#pragma once

#include "core/lua_copy.hpp"
#include "sim/entity_id.hpp"

#include <deque>
#include <optional>
#include <string>
#include <vector>

struct lua_State;

namespace osc::sim {
class SimState;
}

namespace osc::lua {

/// Registry key (UI state, or the sim's) of a pending SetFocusArmy request:
/// 0-based army or -1 for observer. While a world session runs, the UI's
/// SetFocusArmy is a request the next sim beat applies, as Moho's
/// CWldSession does; so is the sim's (FAF's), the UI's winning a tie.
inline constexpr const char* kFocusArmyRequestKey = "__osc_focus_army_request";

/// Registry key (UI state) of a selection the sim hasn't been given yet: an
/// array of entity keys. Moho's CWldSession::SetSelection hands each changed
/// selection to the sim driver (SetSyncFilterMaskB), and the next beat's
/// Sim::Sync copies it into the sim, where DebugGetSelection reads it;
/// SyncQueue carries it, with the focus request.
inline constexpr const char* kDebugSelectionKey = "__osc_debug_selection";

/// The UI state's focus army: 0-based, -1 for an observer (0 when unset).
int focus_army(lua_State* ui_L);

/// The UI's selection changed (Moho's SetSyncFilterMaskB): `ids` for the
/// sim's next Sync, in place of one still pending.
void request_debug_selection(lua_State* ui_L, const std::vector<sim::EntityId>& ids);

/// Set both states' focus army outright, as a game starts (no
/// NoteFocusArmyChanged, no Sync.FocusArmyChanged; either state may be null).
void set_focus_army(lua_State* sim_L, lua_State* ui_L, int army);

/// Plain-data deep copy between states (see core/lua_copy.hpp).
using core::copy_lua_value;

/// The <GameStats> document SubmitXMLArmyStats asks for, as Moho's Sim::Sync
/// builds it (faf-re SimTick.cpp; Steam exe 0x74EF7B): every army's
/// CArmyStats::ArmyXmlStatsNode in turn, indented two spaces. Built from the
/// army stats and the blueprints in `sim_L`, the sim's state; it changes
/// neither.
std::string army_stats_xml(lua_State* sim_L, const sim::SimState& sim);

/// The sim -> user channel's beats (Moho's mSyncDataQueue): each beat's Sync table, copied out
/// of the sim state as the beat ends, waits here for the user side; a save holds none of them.
class SyncQueue {
public:
    SyncQueue();
    ~SyncQueue();
    SyncQueue(const SyncQueue&) = delete;
    SyncQueue& operator=(const SyncQueue&) = delete;

    /// Moho's Sim::Sync, after each beat: the pending selection and focus request applied, a
    /// copy of the sim's Sync queued, with the army stats document if SubmitXMLArmyStats asked
    /// for one (army_stats_xml), then ResetSyncTable(). A null ui_L leaves the UI state alone.
    void queue(lua_State* sim_L, lua_State* ui_L);
    /// Between jobs, take the UI's selection and focus request for the sim thread's next
    /// queue().
    void prepare_threaded(lua_State* sim_L, lua_State* ui_L);
    bool pending() const { return !packets_.empty(); }
    /// Moho's CWldSession::DoBeat: the oldest queued Sync becomes the user state's, then its
    /// OnSync(); with none queued, queue's steps run on the sim's Sync now. Answers the beat's
    /// army stats document (Moho's SSyncData::mSubmitArmyStats), empty for none: the session
    /// hands it to GPGNet after the UI's beat (gpgnet_submit_army_stats).
    std::string deliver(lua_State* sim_L, lua_State* ui_L);
    void clear();

private:
    /// The UI's part of Moho's sync filter, as Sim::Sync takes it in: the selection pending
    /// since the last Sync (Sim::mSyncFilter.maskB), then the focus request (with
    /// NoteFocusArmyChanged). From ui_L, or without it what prepare_threaded took.
    void apply_ui_filter(lua_State* sim_L, lua_State* ui_L);

    lua_State* packets_state_ = nullptr;
    std::deque<int> packets_;
    std::deque<std::vector<std::string>> print_lines_;
    std::deque<std::string> army_stats_; ///< each queued beat's army stats document, or empty
    std::optional<int> ui_request_;
    std::optional<std::vector<sim::EntityId>> ui_selection_;
};

} // namespace osc::lua
