#pragma once

#include "core/types.hpp"
#include "sim/unit_command.hpp"

#include <algorithm>
#include <functional>
#include <map>
#include <unordered_map>
#include <vector>

namespace osc::sim {

/// A player / AI / network command scheduled to execute on a specific sim tick.
/// This is the unit of deterministic replay and lockstep exchange: fed the same
/// stream, every client applies the same orders on the same ticks.
struct ScheduledCommand {
    u32 exec_tick = 0;          // tick at which the order is applied
    u32 source = 0;             // originating command source (player / peer / AI)
    u64 sequence = 0;           // submission order — deterministic tiebreak
    UnitCommand command;        // the order payload
    std::vector<u32> unit_ids;  // entities that receive the order
    bool clear_existing = true; // replace the queue (fresh order) vs append (queued)
};

/// Tick-keyed command buffer for deterministic — and lockstep-ready — command
/// execution. Commands are submitted for a future tick and dispatched in a
/// canonical order when that tick runs.
///
/// Single-player: one local source, non-lockstep, so `ready_to_run` is always
/// true and nothing ever stalls. Multiplayer: set lockstep and every peer must
/// `confirm_frame` each tick (an explicit empty confirmation is allowed) before
/// that tick may run — the classic "waiting for players" gate.
class CommandScheduler {
public:
    /// Submit a command. Its sequence is assigned here (monotonic) and its
    /// source is registered as a participant.
    void submit(ScheduledCommand cmd) {
        register_source(cmd.source);
        cmd.sequence = next_sequence_++;
        by_tick_[cmd.exec_tick].push_back(std::move(cmd));
    }

    /// Register a participating source without submitting a command, so that
    /// `ready_to_run` waits for its confirmations in lockstep mode.
    void add_source(u32 source) { register_source(source); }

    /// Stop treating `source` as a participant (e.g. a dropped peer), so
    /// `ready_to_run` no longer waits on its confirmations.
    void remove_source(u32 source) {
        auto it = std::lower_bound(sources_.begin(), sources_.end(), source);
        if (it != sources_.end() && *it == source) sources_.erase(it);
        confirmed_frame_.erase(source);
    }

    /// Mark that `source` has submitted every command through `tick`.
    void confirm_frame(u32 source, u32 tick) {
        register_source(source);
        u32& c = confirmed_frame_[source];
        if (tick > c) c = tick;
    }

    /// True if the given tick may safely run: in non-lockstep mode always true;
    /// in lockstep mode, true once every known source has confirmed >= tick.
    bool ready_to_run(u32 tick) const {
        if (!lockstep_) return true;
        for (u32 s : sources_) {
            auto it = confirmed_frame_.find(s);
            if (it == confirmed_frame_.end() || it->second < tick) return false;
        }
        return true;
    }

    /// Dispatch (and remove) all commands due at `tick`, in canonical order
    /// (by source, then submission sequence), invoking `apply` for each.
    void dispatch_due(u32 tick,
                      const std::function<void(const ScheduledCommand&)>& apply) {
        auto it = by_tick_.find(tick);
        if (it == by_tick_.end()) return;
        sort_canonical(it->second);
        for (const auto& c : it->second) apply(c);
        by_tick_.erase(it);
    }

    /// Peek the (canonically ordered) commands pending for a tick.
    std::vector<ScheduledCommand> commands_for(u32 tick) const {
        auto it = by_tick_.find(tick);
        if (it == by_tick_.end()) return {};
        std::vector<ScheduledCommand> v = it->second;
        sort_canonical(v);
        return v;
    }

    void set_lockstep(bool on) { lockstep_ = on; }
    bool lockstep() const { return lockstep_; }

    /// Earliest tick that still has pending commands (0 if none).
    u32 next_pending_tick() const {
        return by_tick_.empty() ? 0u : by_tick_.begin()->first;
    }

    size_t pending_count() const {
        size_t n = 0;
        for (const auto& [tick, cmds] : by_tick_) n += cmds.size();
        return n;
    }

    void clear() {
        by_tick_.clear();
        confirmed_frame_.clear();
        sources_.clear();
        next_sequence_ = 0;
    }

private:
    void register_source(u32 s) {
        auto it = std::lower_bound(sources_.begin(), sources_.end(), s);
        if (it == sources_.end() || *it != s) sources_.insert(it, s);
    }

    static void sort_canonical(std::vector<ScheduledCommand>& v) {
        std::stable_sort(v.begin(), v.end(),
                         [](const ScheduledCommand& a, const ScheduledCommand& b) {
                             if (a.source != b.source) return a.source < b.source;
                             return a.sequence < b.sequence;
                         });
    }

    std::map<u32, std::vector<ScheduledCommand>> by_tick_; // ordered by exec tick
    std::unordered_map<u32, u32> confirmed_frame_;          // source -> confirmed tick
    std::vector<u32> sources_;                              // participants (sorted, unique)
    u64 next_sequence_ = 0;
    bool lockstep_ = false;
};

} // namespace osc::sim
