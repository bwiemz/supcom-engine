#include "sim/lockstep_session.hpp"

#include "sim/net_transport.hpp"
#include "sim/sim_state.hpp"

#include <algorithm>
#include <cstring>

#include <spdlog/spdlog.h>

namespace osc::sim {

namespace {

// --- Little-endian frame (de)serialization ---
void put_u8(std::vector<u8>& b, u8 v) { b.push_back(v); }
void put_u32(std::vector<u8>& b, u32 v) {
    b.push_back(static_cast<u8>(v & 0xFF));
    b.push_back(static_cast<u8>((v >> 8) & 0xFF));
    b.push_back(static_cast<u8>((v >> 16) & 0xFF));
    b.push_back(static_cast<u8>((v >> 24) & 0xFF));
}
void put_f32(std::vector<u8>& b, f32 v) {
    u32 bits;
    std::memcpy(&bits, &v, sizeof(bits));
    put_u32(b, bits);
}
void put_str(std::vector<u8>& b, const std::string& s) {
    put_u32(b, static_cast<u32>(s.size()));
    b.insert(b.end(), s.begin(), s.end());
}

struct Reader {
    const std::vector<u8>& b;
    size_t pos = 0;
    bool ok = true;
    bool need(size_t n) {
        if (pos + n > b.size()) ok = false;
        return ok;
    }
    u8 u8v() { return need(1) ? b[pos++] : 0; }
    u32 u32v() {
        if (!need(4)) return 0;
        u32 v = static_cast<u32>(b[pos]) | (static_cast<u32>(b[pos + 1]) << 8) |
                (static_cast<u32>(b[pos + 2]) << 16) |
                (static_cast<u32>(b[pos + 3]) << 24);
        pos += 4;
        return v;
    }
    f32 f32v() {
        u32 bits = u32v();
        f32 v;
        std::memcpy(&v, &bits, sizeof(v));
        return v;
    }
    std::string strv() {
        u32 len = u32v();
        if (!need(len)) return {};
        std::string s(b.begin() + static_cast<long>(pos),
                      b.begin() + static_cast<long>(pos + len));
        pos += len;
        return s;
    }
};

void serialize_command(std::vector<u8>& b, const ScheduledCommand& c) {
    put_u32(b, c.exec_tick);
    put_u32(b, c.source);
    put_u8(b, c.clear_existing ? 1 : 0);
    put_u8(b, static_cast<u8>(c.command.type));
    put_f32(b, c.command.target_pos.x);
    put_f32(b, c.command.target_pos.y);
    put_f32(b, c.command.target_pos.z);
    put_u32(b, c.command.target_id);
    put_u32(b, c.command.command_id);
    put_str(b, c.command.blueprint_id);
    put_u32(b, static_cast<u32>(c.unit_ids.size()));
    for (u32 id : c.unit_ids) put_u32(b, id);
}

ScheduledCommand read_command(Reader& r) {
    ScheduledCommand c;
    c.exec_tick = r.u32v();
    c.source = r.u32v();
    c.clear_existing = r.u8v() != 0;
    c.command.type = static_cast<CommandType>(r.u8v());
    c.command.target_pos.x = r.f32v();
    c.command.target_pos.y = r.f32v();
    c.command.target_pos.z = r.f32v();
    c.command.target_id = r.u32v();
    c.command.command_id = r.u32v();
    c.command.blueprint_id = r.strv();
    u32 n = r.u32v();
    c.unit_ids.reserve(n);
    for (u32 i = 0; i < n && r.ok; ++i) c.unit_ids.push_back(r.u32v());
    return c;
}

} // namespace

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

void LockstepSession::send_frame() {
    std::vector<u8> msg;
    put_u32(msg, local_source_);
    put_u32(msg, next_frame_);

    // Attach the most recent executed-tick checksum for desync detection.
    u32 last_tick = sim_.tick_count();
    auto it = my_checksums_.find(last_tick);
    if (last_tick > 0 && it != my_checksums_.end()) {
        put_u8(msg, 1);
        put_u32(msg, last_tick);
        put_u32(msg, it->second);
    } else {
        put_u8(msg, 0);
        put_u32(msg, 0);
        put_u32(msg, 0);
    }

    put_u32(msg, static_cast<u32>(pending_.size()));
    for (const auto& c : pending_) serialize_command(msg, c);
    transport_.broadcast(msg);

    sim_.command_scheduler().confirm_frame(local_source_, next_frame_);
    pending_.clear();
    ++next_frame_;
}

void LockstepSession::receive_and_advance() {
    for (const auto& raw : transport_.receive()) {
        Reader r{raw};
        u32 source = r.u32v();
        u32 frame = r.u32v();
        u8 has_cs = r.u8v();
        u32 cs_tick = r.u32v();
        u32 cs_val = r.u32v();
        u32 count = r.u32v();
        for (u32 i = 0; i < count && r.ok; ++i) {
            sim_.command_scheduler().submit(read_command(r));
        }
        if (!r.ok) continue; // malformed frame — ignore
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
