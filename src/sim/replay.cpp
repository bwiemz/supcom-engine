#include "sim/replay.hpp"
#include "sim/command_codec.hpp"


namespace osc::sim {


std::vector<u8> Replay::serialize() const {
    std::vector<u8> b;
    ByteWriter w(b);
    for (char c : {'O', 'S', 'C', 'R'}) w.u8v(static_cast<u8>(c));
    w.u32v(version);
    w.u32v(final_tick);
    w.u32v(command_delay);
    w.u64v(seed);
    w.str(victory_condition);
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
    const u32 count = r.u32v();
    for (u32 i = 0; i < count && r.ok(); ++i) {
        ScheduledCommand c;
        if (read_command(r, c)) out.commands.push_back(std::move(c));
    }
    if (!r.ok()) {
        out = Replay{};
        return false;
    }
    return true;
}

} // namespace osc::sim
