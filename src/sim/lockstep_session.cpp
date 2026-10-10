#include "sim/lockstep_session.hpp"
#include "sim/command_codec.hpp"

#include "sim/net_transport.hpp"
#include "sim/sim_state.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

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
    sim_.set_pause_holds(true); // until a peer resumes it
}

void LockstepSession::request_pause() {
    if (sim_.network_paused()) return; // (Moho's sim ignores it too)
    SimCallbackEntry pause;
    pause.func_name = kRequestPauseCallback;
    submit_local_callback(std::move(pause));
}

bool LockstepSession::request_resume() {
    if (!sim_.network_paused()) return false;
    const u32 serial = sim_.pause_serial();
    std::vector<u8> msg;
    ByteWriter w(msg);
    w.u8v(kResumeMessage);
    w.u32v(local_source_);
    w.u32v(serial);
    transport_.broadcast(msg);
    sim_.resume_pause(serial);
    return true;
}

void LockstepSession::set_speed_option(i32 rate, bool adjustable) {
    speed_ = rate;
    adjustable_speed_ = adjustable;
}

bool LockstepSession::request_speed(i32 rate) {
    if (!adjustable_speed_) return false;
    const u32 clock = speed_clock_ + 1;
    std::vector<u8> msg;
    ByteWriter w(msg);
    w.u8v(kSpeedMessage);
    w.u32v(local_source_);
    w.u32v(clock);
    w.u32v(static_cast<u32>(rate));
    transport_.broadcast(msg);
    apply_speed(clock, local_source_, rate); // as Moho's, which hears its own
    return true;
}

void LockstepSession::apply_speed(u32 clock, u32 source, i32 rate) {
    if (clock < speed_clock_ || (clock == speed_clock_ && source >= speed_requester_)) return;
    speed_clock_ = clock;
    speed_requester_ = source;
    speed_ = std::clamp(rate, -10, 50); // whoever asked (a peer's message too)
    speed_changes_.push_back({source, speed_});
}

std::vector<LockstepSession::SpeedChange> LockstepSession::take_speed_changes() {
    return std::exchange(speed_changes_, {});
}

std::vector<LockstepSession::Desync> LockstepSession::take_desyncs() {
    return std::exchange(desyncs_, {});
}

namespace {

/// How many rounds `normal` of them at normal speed are, at `speed` (its
/// scale held to 0.1..10: the frame loop runs a few rounds a frame at most)
u32 scaled_rounds(u32 normal, i32 speed) {
    const double scale = std::pow(10.0, std::clamp(speed, -10, 10) * 0.1);
    return std::max<u32>(1, static_cast<u32>(std::lround(normal * scale)));
}

} // namespace

u32 LockstepSession::lead_cap() const {
    return scaled_rounds(kMaxLead, speed_);
}

u32 LockstepSession::drop_timeout() const {
    return drop_timeout_rounds_ == 0 ? 0 : scaled_rounds(drop_timeout_rounds_, speed_);
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
    // Paused, no frames: each confirms a tick, and would run at once on
    // resuming (its commands wait in pending_)
    if (sim_.network_paused()) return;
    ++round_;
    std::vector<u8> msg;
    ByteWriter w(msg);
    // As far ahead of the sim as a peer runs: it waits for the others (its
    // orders wait in pending_), saying only that it is still there
    if (next_frame_ > sim_.tick_count() + lead_cap()) {
        w.u8v(kAliveMessage);
        w.u32v(local_source_);
        transport_.broadcast(msg);
        return;
    }
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
        if (type == kResumeMessage) {
            const u32 source = r.u32v();
            const u32 serial = r.u32v();
            if (r.ok() && source != local_source_ && !has_dropped(source))
                sim_.resume_pause(serial);
            continue;
        }
        if (type == kSpeedMessage) {
            const u32 source = r.u32v();
            const u32 clock = r.u32v();
            const auto rate = static_cast<i32>(r.u32v());
            if (r.ok() && source != local_source_ && !has_dropped(source))
                apply_speed(clock, source, rate);
            continue;
        }
        if (type == kAliveMessage) {
            const u32 source = r.u32v();
            auto heard = peer_heard_round_.find(source);
            if (r.ok() && heard != peer_heard_round_.end()) heard->second = round_;
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
        if (frame > pc) pc = frame;
        peer_heard_round_[source] = round_; // arms the drop timer after first contact
        if (has_cs) note_peer_checksum(source, cs_tick, cs_parts);
        // Kept to relay, should this peer drop before every survivor has it.
        auto& kept = recent_frames_[source];
        kept[frame] = std::move(commands);
        while (!kept.empty() && kept.begin()->first + kRelayFrames < frame)
            kept.erase(kept.begin());
    }

    // Ejected, this peer's game is over: it neither ticks on nor drops the
    // others (cut off from them, it would time them all out and play on
    // alone), as Moho's ejected client waits
    if (ejected_) return;
    // Paused: no frames flow, so none is late; nothing ticks
    if (sim_.network_paused()) return;

    // A peer silent for longer than the timeout, in rounds (each a tick's
    // time: a peer held at kMaxLead still has them, with no frames), is
    // reported by this survivor; the drop happens once every survivor has.
    // Only sources that have sent a frame are armed: peers load the game at
    // their own speed, so a slow first frame at session start can't
    // false-drop. One that never sends one is left to the players, as
    // Moho's: the disconnect dialog shows it, and they eject it.
    if (const u32 timeout = drop_timeout(); timeout > 0) {
        std::vector<u32> late;
        for (const auto& [src, heard] : peer_heard_round_) {
            if (src == local_source_ || has_dropped(src) || dropping(src)) continue;
            if (round_ - heard > timeout) late.push_back(src);
        }
        std::sort(late.begin(), late.end());
        for (u32 src : late) {
            spdlog::warn("[lockstep] peer source {} timed out ({} rounds without a word)", src,
                         round_ - peer_heard_round_[src]);
            begin_drop(src);
        }
    }
    finalize_drops();

    // Advance as far as every peer's confirmations allow (a pause taken on
    // a tick holds before the next).
    while (!sim_.network_paused() && sim_.command_scheduler().ready_to_run(sim_.tick_count() + 1)) {
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
        if (!ejected_)
            spdlog::error("[lockstep] peer {} reports this peer as dropped; the game can't "
                          "continue in sync",
                          reporter);
        ejected_ = true;
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
    for (const auto& reporter : vote.last_frame) dropped_by_[source].push_back(reporter.first);
    newly_dropped_.push_back(source);
    recent_frames_.erase(source);

    // Its army -- the one its source plays, which a lobby's game numbers
    // apart from its source (an observer's plays none) -- is defeated on the
    // tick after its last frame, on every survivor, by the engine rather
    // than any player.
    const i32 army = sim_.army_of_source(source);
    if (army < 0) {
        spdlog::warn("[lockstep] peer source {} (watching) dropped after its frame {}", source,
                     last);
        return;
    }
    ScheduledCommand defeat;
    defeat.exec_tick = last + 1;
    defeat.source = kEngineSource;
    defeat.callback = SimCallbackEntry{};
    defeat.callback->func_name = kDefeatArmyCallback;
    defeat.callback->args["Army"] = static_cast<f64>(army);
    sim_.command_scheduler().submit(std::move(defeat));
    spdlog::warn("[lockstep] peer source {} dropped after its frame {}; its army {} is defeated "
                 "at tick {}",
                 source, last, army, last + 1);
}

std::vector<u32> LockstepSession::take_dropped() {
    std::vector<u32> out;
    out.swap(newly_dropped_);
    return out;
}

bool LockstepSession::eject(u32 source) {
    if (source == local_source_ || has_dropped(source) || dropping(source) ||
        std::find(all_sources_.begin(), all_sources_.end(), source) == all_sources_.end())
        return false;
    spdlog::warn("[lockstep] ejecting peer source {}", source);
    begin_drop(source);
    finalize_drops(); // this peer may be its only survivor
    return true;
}

std::vector<u32> LockstepSession::ejectors(u32 source) const {
    std::vector<u32> out;
    if (const auto vote = drop_votes_.find(source); vote != drop_votes_.end()) {
        for (const auto& reporter : vote->second.last_frame) out.push_back(reporter.first);
    } else if (const auto by = dropped_by_.find(source); by != dropped_by_.end()) {
        out = by->second;
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool LockstepSession::has_dropped(u32 src) const {
    return std::find(dropped_.begin(), dropped_.end(), src) != dropped_.end();
}

void LockstepSession::note_peer_checksum(u32 source, u32 tick, const Parts& parts) {
    peer_checksums_[tick][source] = parts;
    auto it = my_checksums_.find(tick);
    if (it != my_checksums_.end()) {
        compare_checksums(tick, source, it->second, parts);
    }
}

void LockstepSession::record_local_checksum(u32 tick, const Parts& parts) {
    my_checksums_[tick] = parts;
    auto it = peer_checksums_.find(tick);
    if (it == peer_checksums_.end()) {
        return;
    }
    for (const auto& [source, theirs] : it->second) {
        compare_checksums(tick, source, parts, theirs);
    }
}

void LockstepSession::compare_checksums(u32 tick, u32 source, const Parts& mine,
                                        const Parts& theirs) {
    if (mine == theirs) {
        return;
    }
    desyncs_.push_back({tick, source});
    if (desynced_) {
        return;
    }
    desynced_ = true;
    desync_tick_ = tick;
    const auto fold = [](const Parts& parts) {
        u64 h = 0xcbf29ce484222325ull;
        for (const u64 part : parts) h = (h ^ part) * 0x100000001b3ull;
        return h;
    };
    desync_hashes_ = {fold(mine), fold(theirs)};
    std::string names;
    for (size_t i = 0; i < mine.size(); ++i) {
        if (mine[i] == theirs[i]) continue;
        desync_domains_.emplace_back(SimState::ChecksumParts::kNames[i]);
        names += (names.empty() ? "" : ", ") + desync_domains_.back();
    }
    spdlog::error("[lockstep] desync at tick {}: {} differ", tick, names);
}

} // namespace osc::sim
