#include "sim/replay.hpp"

#include <cstring>

namespace osc::sim {

namespace {

// --- Little-endian byte writer ---
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

// --- Little-endian byte reader with bounds checks ---
struct Reader {
    const std::vector<u8>& b;
    size_t pos = 0;
    bool ok = true;

    bool need(size_t n) {
        if (pos + n > b.size()) ok = false;
        return ok;
    }
    u8 u8v() {
        if (!need(1)) return 0;
        return b[pos++];
    }
    u32 u32v() {
        if (!need(4)) return 0;
        u32 v = static_cast<u32>(b[pos]) |
                (static_cast<u32>(b[pos + 1]) << 8) |
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

} // namespace

std::vector<u8> Replay::serialize() const {
    std::vector<u8> b;
    b.push_back('O');
    b.push_back('S');
    b.push_back('C');
    b.push_back('R');
    put_u32(b, version);
    put_u32(b, final_tick);
    put_u32(b, command_delay);
    put_str(b, victory_condition);
    put_u32(b, static_cast<u32>(commands.size()));
    for (const auto& c : commands) {
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
    return b;
}

bool Replay::deserialize(const std::vector<u8>& bytes, Replay& out) {
    out = Replay{};
    Reader r{bytes};
    if (r.u8v() != 'O' || r.u8v() != 'S' || r.u8v() != 'C' || r.u8v() != 'R')
        return false;
    out.version = r.u32v();
    if (!r.ok || out.version != kVersion) {
        out = Replay{};
        return false;
    }
    out.final_tick = r.u32v();
    out.command_delay = r.u32v();
    out.victory_condition = r.strv();
    u32 count = r.u32v();
    out.commands.reserve(count);
    for (u32 i = 0; i < count && r.ok; ++i) {
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
        for (u32 j = 0; j < n && r.ok; ++j) c.unit_ids.push_back(r.u32v());
        out.commands.push_back(std::move(c));
    }
    if (!r.ok) {
        out = Replay{};
        return false;
    }
    return true;
}

} // namespace osc::sim
