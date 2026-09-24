#include "sim/lockstep_session.hpp"
#include "sim/command_codec.hpp"

#include "sim/net_transport.hpp"
#include "sim/sim_state.hpp"

#include <algorithm>
#include <limits>

#include <spdlog/spdlog.h>

namespace osc::sim {


LockstepSession::LockstepSession(SimState& sim, INetTransport& transport, u32 local_source,
                                 const std::vector<u32>& all_sources)
    : sim_(sim), transport_(transport), local_source_(local_source), all_sources_(all_sources) {
    if (std::find(all_sources_.begin(), all_sources_.end(), local_source) == all_sources_.end())
        all_sources_.push_back(local_source);
    std::sort(all_sources_.begin(), all_sources_.end());
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
    w.u8v(kFrameMessage);
    w.u32v(local_source_);
    w.u32v(next_frame_);

    // Attach the most recent executed-tick checksum for desync detection.
    u32 last_tick = sim_.tick_count();
    auto it = my_checksums_.find(last_tick);
    // By domain, so a peer that differs can say what differs.
    if (last_tick > 0 && it != my_checksums_.end()) {
        w.u8v(1);
        w.u32v(last_tick);
        for (const u64 part : it->second) w.u64v(part);
    } else {
        w.u8v(0);
        w.u32v(0);
        for (size_t i = 0; i < SimState::ChecksumParts::kCount; ++i) w.u64v(0);
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
        const u8 type = r.u8v();
        if (type == kDropMessage) {
            take_drop_report(r);
            continue;
        }
        if (type != kFrameMessage) continue;
        u32 source = r.u32v();
        // Ignore a source that is dropped or being dropped: late frames must
        // neither re-register it in the scheduler gate (re-stalling the
        // survivors) nor move the last frame this peer reported for it.
        if (has_dropped(source) || dropping(source) || source == local_source_) continue;
        u32 frame = r.u32v();
        u8 has_cs = r.u8v();
        u32 cs_tick = r.u32v();
        Parts cs_parts{};
        for (u64& part : cs_parts) part = r.u64v();
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
        if (!r.ok()) continue; // malformed frame — ignore
        for (const auto& c : commands) sim_.command_scheduler().submit(c);
        sim_.command_scheduler().confirm_frame(source, frame);
        u32& pc = peer_confirmed_[source];
        if (frame > pc) pc = frame; // arms the drop timer after first contact
        if (has_cs) note_peer_checksum(cs_tick, cs_parts);
        // Kept to relay, should this peer drop before every survivor has it.
        auto& kept = recent_frames_[source];
        kept[frame] = std::move(commands);
        while (!kept.empty() && kept.begin()->first + kRelayFrames < frame)
            kept.erase(kept.begin());
    }

    // A peer that has gone silent falls further behind each round (next_frame_
    // keeps advancing while its confirmed frame is frozen). Past the timeout,
    // this survivor reports it; the drop happens once every survivor has.
    // Only sources that have confirmed at least one frame are armed, so a slow
    // first frame at session start can't false-drop.
    if (drop_timeout_frames_ > 0) {
        std::vector<u32> late;
        for (const auto& [src, confirmed] : peer_confirmed_) {
            if (src == local_source_ || has_dropped(src) || dropping(src)) continue;
            const u32 behind = next_frame_ > confirmed ? next_frame_ - confirmed : 0;
            if (behind > drop_timeout_frames_) late.push_back(src);
        }
        std::sort(late.begin(), late.end());
        for (u32 src : late) {
            spdlog::warn("[lockstep] peer source {} timed out ({} frames behind)", src,
                         next_frame_ - peer_confirmed_[src]);
            begin_drop(src);
        }
    }
    finalize_drops();

    // Advance as far as every peer's confirmations allow.
    while (sim_.command_scheduler().ready_to_run(sim_.tick_count() + 1)) {
        sim_.tick();
        record_local_checksum(sim_.tick_count(), sim_.tick_checksum().values());
    }
}

void LockstepSession::begin_drop(u32 source) {
    if (source == local_source_ || has_dropped(source) || dropping(source)) return;
    DropVote& vote = drop_votes_[source];
    auto confirmed = peer_confirmed_.find(source);
    const u32 last = confirmed == peer_confirmed_.end() ? 0 : confirmed->second;
    vote.last_frame[local_source_] = last;
    auto kept = recent_frames_.find(source);
    if (kept != recent_frames_.end()) vote.frames = kept->second;

    // Report it: the last frame this peer holds, and the frames it has kept,
    // as many of the newest as fit one wire message (the newest are the ones
    // another survivor may have missed; a message over the limit would get
    // this peer dropped as hostile).
    std::vector<std::vector<u8>> encoded;
    size_t room = kMaxWireMessage - 64; // the report's header
    for (auto it = vote.frames.rbegin(); it != vote.frames.rend(); ++it) {
        std::vector<u8> one;
        ByteWriter fw(one);
        fw.u32v(it->first);
        fw.u32v(static_cast<u32>(it->second.size()));
        for (const auto& c : it->second) write_command(fw, c);
        if (one.size() > room) break;
        room -= one.size();
        encoded.push_back(std::move(one));
    }
    if (encoded.size() < vote.frames.size())
        spdlog::warn("[lockstep] drop report for source {} relays its newest {} of {} frames",
                     source, encoded.size(), vote.frames.size());
    std::vector<u8> msg;
    ByteWriter w(msg);
    w.u8v(kDropMessage);
    w.u32v(local_source_);
    w.u32v(source);
    w.u32v(last);
    w.u32v(static_cast<u32>(encoded.size()));
    for (auto it = encoded.rbegin(); it != encoded.rend(); ++it)
        msg.insert(msg.end(), it->begin(), it->end());
    transport_.broadcast(msg);
    spdlog::warn("[lockstep] dropping peer source {}: reported its frame {} to the survivors",
                 source, last);
}

void LockstepSession::take_drop_report(ByteReader& r) {
    const u32 reporter = r.u32v();
    const u32 source = r.u32v();
    const u32 last = r.u32v();
    const u32 frames = r.u32v();
    // Parse it whole first, as a frame.
    std::map<u32, std::vector<ScheduledCommand>> relayed;
    for (u32 i = 0; i < frames && r.ok(); ++i) {
        const u32 frame = r.u32v();
        const u32 count = r.u32v();
        std::vector<ScheduledCommand> commands;
        for (u32 j = 0; j < count && r.ok(); ++j) {
            ScheduledCommand c;
            if (read_command(r, c) && c.source == source) commands.push_back(std::move(c));
            else r.fail();
        }
        relayed.emplace(frame, std::move(commands));
    }
    if (!r.ok() || reporter == local_source_ || reporter == source) return;
    if (source == local_source_) {
        spdlog::error("[lockstep] peer {} reports this peer as dropped; the game can't "
                      "continue in sync",
                      reporter);
        return;
    }
    if (has_dropped(source) || has_dropped(reporter) || dropping(reporter)) return;
    begin_drop(source); // a report from another survivor draws this one's
    DropVote& vote = drop_votes_[source];
    vote.last_frame[reporter] = last;
    for (auto& [frame, commands] : relayed) vote.frames.emplace(frame, std::move(commands));
}

void LockstepSession::finalize_drops() {
    // A drop is agreed once every survivor -- every participant neither
    // dropped nor being dropped -- has reported it. Finishing one can
    // complete another (its survivors shrink), so go round until none do.
    bool progress = true;
    while (progress) {
        progress = false;
        for (auto it = drop_votes_.begin(); it != drop_votes_.end(); ++it) {
            bool complete = true;
            for (u32 s : all_sources_) {
                if (has_dropped(s) || dropping(s)) continue;
                if (!it->second.last_frame.count(s)) {
                    complete = false;
                    break;
                }
            }
            if (!complete) continue;
            const u32 source = it->first;
            const DropVote vote = std::move(it->second);
            drop_votes_.erase(it);
            finalize_drop(source, vote);
            progress = true;
            break;
        }
    }
}

void LockstepSession::finalize_drop(u32 source, const DropVote& vote) {
    u32 last = 0;
    for (const auto& [reporter, frame] : vote.last_frame) last = std::max(last, frame);
    // This peer ran no tick past the frames it holds from the dropped one,
    // and no survivor holds more than `last`: every survivor applies the
    // same frames, up to `last`, before its defeat.
    auto confirmed = peer_confirmed_.find(source);
    const u32 held = confirmed == peer_confirmed_.end() ? 0 : confirmed->second;
    // Walk the frames the reports carried, not the frame numbers up to
    // `last`: that is only a peer's word, and must not set how long this
    // runs. (Nor may the defeat's tick wrap around.)
    last = std::min(last, std::numeric_limits<u32>::max() - 1);
    u64 applied = 0;
    for (auto it = vote.frames.upper_bound(held); it != vote.frames.end() && it->first <= last;
         ++it, ++applied) {
        for (const auto& c : it->second) sim_.command_scheduler().submit(c);
    }
    if (last > held && applied < static_cast<u64>(last - held)) {
        spdlog::error("[lockstep] {} frames of dropped peer {} up to frame {} reached no "
                      "survivor that kept them; the game may desync",
                      static_cast<u64>(last - held) - applied, source, last);
    }
    sim_.command_scheduler().confirm_frame(source, last);
    sim_.command_scheduler().remove_source(source);
    dropped_.push_back(source);
    newly_dropped_.push_back(source);
    recent_frames_.erase(source);

    // Its army is defeated on the tick after its last frame, on every
    // survivor, by the engine rather than any player.
    ScheduledCommand defeat;
    defeat.exec_tick = last + 1;
    defeat.source = kEngineSource;
    defeat.callback = SimCallbackEntry{};
    defeat.callback->func_name = kDefeatArmyCallback;
    defeat.callback->args["Army"] = static_cast<f64>(source);
    sim_.command_scheduler().submit(std::move(defeat));
    spdlog::warn("[lockstep] peer source {} dropped after its frame {}; its army is defeated "
                 "at tick {}",
                 source, last, last + 1);
}

std::vector<u32> LockstepSession::take_dropped() {
    std::vector<u32> out;
    out.swap(newly_dropped_);
    return out;
}

bool LockstepSession::has_dropped(u32 src) const {
    return std::find(dropped_.begin(), dropped_.end(), src) != dropped_.end();
}

void LockstepSession::note_peer_checksum(u32 tick, const Parts& parts) {
    peer_checksums_[tick] = parts;
    auto it = my_checksums_.find(tick);
    if (it != my_checksums_.end()) compare_checksums(tick, it->second, parts);
}

void LockstepSession::record_local_checksum(u32 tick, const Parts& parts) {
    my_checksums_[tick] = parts;
    auto it = peer_checksums_.find(tick);
    if (it != peer_checksums_.end()) compare_checksums(tick, parts, it->second);
}

void LockstepSession::compare_checksums(u32 tick, const Parts& mine, const Parts& theirs) {
    if (mine == theirs || desynced_) return;
    desynced_ = true;
    desync_tick_ = tick;
    std::string names;
    for (size_t i = 0; i < mine.size(); ++i) {
        if (mine[i] == theirs[i]) continue;
        desync_domains_.emplace_back(SimState::ChecksumParts::kNames[i]);
        names += (names.empty() ? "" : ", ") + desync_domains_.back();
    }
    spdlog::error("[lockstep] desync at tick {}: {} differ", tick, names);
}

} // namespace osc::sim
