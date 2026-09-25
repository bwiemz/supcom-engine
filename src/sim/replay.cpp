#include "sim/replay.hpp"
#include "sim/command_codec.hpp"
#include "sim/sim_state.hpp"


namespace osc::sim {


std::vector<u8> Replay::serialize() const {
    std::vector<u8> b;
    ByteWriter w(b);
    for (char c : {'O', 'S', 'C', 'R'}) w.u8v(static_cast<u8>(c));
    w.u32v(kVersion); // the layout written below, whatever version was read
    w.u32v(final_tick);
    w.u32v(command_delay);
    w.u64v(seed);
    w.str(victory_condition);
    w.str(build);
    w.u8v(has_setup ? 1 : 0);
    if (has_setup) write_game_setup(w, setup);
    w.u32v(checksum_from);
    w.u32v(static_cast<u32>(checksums.size()));
    for (u32 c : checksums) w.u32v(c);
    w.u32v(static_cast<u32>(commands.size()));
    for (const auto& c : commands) write_command(w, c);
    return b;
}

bool Replay::deserialize(const std::vector<u8>& bytes, Replay& out) {
    out = Replay{};
    ByteReader r(bytes);
    if (r.u8v() != 'O' || r.u8v() != 'S' || r.u8v() != 'C' || r.u8v() != 'R')
        return false;
    out.version = r.u32v();
    // Version 1 has no seed: it replays with the default one.
    if (!r.ok() || out.version < 1 || out.version > kVersion) {
        out = Replay{};
        return false;
    }
    out.final_tick = r.u32v();
    out.command_delay = r.u32v();
    if (out.version >= 2) out.seed = r.u64v();
    out.victory_condition = r.str();
    if (out.version >= 4) {
        out.build = r.str();
        out.has_setup = r.u8v() != 0;
        if (out.has_setup) read_game_setup(r, out.setup);
        out.checksum_from = r.u32v();
        const u32 trail = r.u32v();
        for (u32 i = 0; i < trail && r.ok(); ++i) out.checksums.push_back(r.u32v());
    }
    const u32 count = r.u32v();
    for (u32 i = 0; i < count && r.ok(); ++i) {
        ScheduledCommand c;
        if (read_command(r, c, /*with_callback=*/out.version >= 3,
                         /*with_formation=*/out.version >= 5, /*with_unload=*/out.version >= 6,
                         /*with_factory=*/out.version >= 7))
            out.commands.push_back(std::move(c));
    }
    if (!r.ok()) {
        out = Replay{};
        return false;
    }
    return true;
}

void ReplayPlayback::start(SimState& sim) const {
    sim.set_playback(true);
    sim.queue_replay(replay_);
}

bool ReplayPlayback::check(const SimState& sim) {
    if (diverged_at_ != 0) return false;
    const u32 tick = sim.tick_count();
    if (tick < replay_.checksum_from) return true;
    const u64 i = tick - replay_.checksum_from;
    if (i >= replay_.checksums.size()) return true; // past the recording
    if (sim.compute_sync_checksum() == replay_.checksums[i]) return true;
    diverged_at_ = tick;
    return false;
}

bool ReplayPlayback::finished(const SimState& sim) const {
    return sim.tick_count() >= replay_.final_tick;
}

} // namespace osc::sim
