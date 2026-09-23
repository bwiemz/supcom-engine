#include "sim/lockstep_session.hpp"
#include "sim/command_codec.hpp"

#include "sim/net_transport.hpp"
#include "sim/sim_state.hpp"

#include <algorithm>

#include <spdlog/spdlog.h>

namespace osc::sim {


LockstepSession::LockstepSession(SimState& sim, INetTransport& transport,
                                 u32 local_source,
                                 const std::vector<u32>& all_sources)
    : sim_(sim), transport_(transport), local_source_(local_source) {
    sim_.command_scheduler().set_lockstep(true);
    for (u32 s : all_sources) sim_.command_scheduler().add_source(s);
    sim_.command_scheduler().add_source(local_source);
}

void LockstepSession::submit_local(const std::vector<u32>& unit_ids,
                                   const UnitCommand& cmd, bool clear_existing) {
    ScheduledCommand sc;
    sc.exec_tick = next_frame_;
    sc.source = local_source_;
    sc.command = cmd;
    sc.unit_ids = unit_ids;
    sc.clear_existing = clear_existing;
    sim_.command_scheduler().submit(sc); // apply to local sim
    pending_.push_back(std::move(sc));    // and queue for broadcast
}

void LockstepSession::submit_local_callback(SimCallbackEntry callback) {
    ScheduledCommand sc;
    sc.exec_tick = next_frame_;
    sc.source = local_source_;
    sc.callback = std::move(callback);
    sim_.command_scheduler().submit(sc); // apply to local sim
    pending_.push_back(std::move(sc));   // and queue for broadcast
}

void LockstepSession::send_frame() {
    std::vector<u8> msg;
    ByteWriter w(msg);
    w.u32v(local_source_);
    w.u32v(next_frame_);

    // Attach the most recent executed-tick checksum for desync detection.
    u32 last_tick = sim_.tick_count();
    auto it = my_checksums_.find(last_tick);
    if (last_tick > 0 && it != my_checksums_.end()) {
        w.u8v(1);
        w.u32v(last_tick);
        w.u32v(it->second);
    } else {
        w.u8v(0);
        w.u32v(0);
        w.u32v(0);
    }

    w.u32v(static_cast<u32>(pending_.size()));
    for (const auto& c : pending_) write_command(w, c);
    transport_.broadcast(msg);

    sim_.command_scheduler().confirm_frame(local_source_, next_frame_);
    pending_.clear();
    ++next_frame_;
}

void LockstepSession::receive_and_advance() {
    for (const auto& raw : transport_.receive()) {
        ByteReader r(raw);
        u32 source = r.u32v();
        // Ignore any late/buffered data from a source we already dropped —
        // re-registering it (via submit/confirm_frame) would put it back in the
        // scheduler gate and re-stall the survivor.
        if (has_dropped(source)) continue;
        u32 frame = r.u32v();
        u8 has_cs = r.u8v();
        u32 cs_tick = r.u32v();
        u32 cs_val = r.u32v();
        u32 count = r.u32v();
        // Parse the whole frame before applying any of it: a malformed frame
        // must not leave half its commands scheduled.
        std::vector<ScheduledCommand> commands;
        for (u32 i = 0; i < count && r.ok(); ++i) {
            ScheduledCommand c;
            // A peer speaks only for itself: a command in its frame that
            // claims another source is forged or corrupt.
            if (read_command(r, c) && c.source == source) commands.push_back(std::move(c));
            else r.fail();
        }
        // Malformed, or claiming to be this peer: ignore the whole frame.
        if (!r.ok() || source == local_source_) continue;
        for (auto& c : commands) sim_.command_scheduler().submit(std::move(c));
        sim_.command_scheduler().confirm_frame(source, frame);
        u32& pc = peer_confirmed_[source];
        if (frame > pc) pc = frame; // arms the drop timer after first contact
        if (has_cs) note_peer_checksum(cs_tick, cs_val);
    }

    // Advance as far as every peer's confirmations allow.
    while (sim_.command_scheduler().ready_to_run(sim_.tick_count() + 1)) {
        sim_.tick();
        record_local_checksum(sim_.tick_count(), sim_.compute_sync_checksum());
    }

    // A peer that has gone silent falls further behind each round (next_frame_
    // keeps advancing while its confirmed frame is frozen). Past the timeout,
    // declare it dropped and stop the scheduler waiting on it. Only sources that
    // have confirmed at least one frame are armed, so a slow first frame at
    // session start can't false-drop.
    if (drop_timeout_frames_ > 0) {
        for (const auto& [src, confirmed] : peer_confirmed_) {
            if (src == local_source_) continue;
            if (std::find(dropped_.begin(), dropped_.end(), src) != dropped_.end())
                continue;
            u32 behind = next_frame_ > confirmed ? next_frame_ - confirmed : 0;
            if (behind > drop_timeout_frames_) {
                dropped_.push_back(src);
                newly_dropped_.push_back(src);
                sim_.command_scheduler().remove_source(src);
                spdlog::warn("[lockstep] peer source {} timed out ({} frames "
                             "behind) — dropped",
                             src, behind);
            }
        }
    }
}

std::vector<u32> LockstepSession::take_dropped() {
    std::vector<u32> out;
    out.swap(newly_dropped_);
    return out;
}

bool LockstepSession::has_dropped(u32 src) const {
    return std::find(dropped_.begin(), dropped_.end(), src) != dropped_.end();
}

void LockstepSession::note_peer_checksum(u32 tick, u32 checksum) {
    peer_checksums_[tick] = checksum;
    auto it = my_checksums_.find(tick);
    if (it != my_checksums_.end() && it->second != checksum) desynced_ = true;
}

void LockstepSession::record_local_checksum(u32 tick, u32 checksum) {
    my_checksums_[tick] = checksum;
    auto it = peer_checksums_.find(tick);
    if (it != peer_checksums_.end() && it->second != checksum) desynced_ = true;
}

} // namespace osc::sim
