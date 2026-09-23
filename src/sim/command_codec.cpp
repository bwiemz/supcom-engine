#include "sim/command_codec.hpp"

#include <cstring>

namespace osc::sim {

void ByteWriter::u32v(u32 v) {
    for (int i = 0; i < 4; ++i) b_.push_back(static_cast<u8>((v >> (8 * i)) & 0xFF));
}

void ByteWriter::u64v(u64 v) {
    u32v(static_cast<u32>(v));
    u32v(static_cast<u32>(v >> 32));
}

void ByteWriter::f32v(f32 v) {
    u32 bits;
    std::memcpy(&bits, &v, sizeof(bits));
    u32v(bits);
}

void ByteWriter::f64v(f64 v) {
    u64 bits;
    std::memcpy(&bits, &v, sizeof(bits));
    u64v(bits);
}

void ByteWriter::str(const std::string& s) {
    u32v(static_cast<u32>(s.size()));
    b_.insert(b_.end(), s.begin(), s.end());
}

bool ByteReader::need(size_t n) {
    if (!ok_ || n > b_.size() - pos_) ok_ = false;
    return ok_;
}

u8 ByteReader::u8v() {
    return need(1) ? b_[pos_++] : 0;
}

u32 ByteReader::u32v() {
    if (!need(4)) return 0;
    u32 v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<u32>(b_[pos_ + static_cast<size_t>(i)]) << (8 * i);
    pos_ += 4;
    return v;
}

u64 ByteReader::u64v() {
    const u64 lo = u32v();
    const u64 hi = u32v();
    return lo | (hi << 32);
}

f32 ByteReader::f32v() {
    const u32 bits = u32v();
    f32 v;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}

f64 ByteReader::f64v() {
    const u64 bits = u64v();
    f64 v;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}

std::string ByteReader::str() {
    const u32 len = u32v();
    if (!need(len)) return {};
    std::string s(b_.begin() + static_cast<std::ptrdiff_t>(pos_),
                  b_.begin() + static_cast<std::ptrdiff_t>(pos_ + len));
    pos_ += len;
    return s;
}

void write_command(ByteWriter& w, const ScheduledCommand& c) {
    w.u32v(c.exec_tick);
    w.u32v(c.source);
    w.u8v(c.clear_existing ? 1 : 0);
    w.u8v(static_cast<u8>(c.command.type));
    w.f32v(c.command.target_pos.x);
    w.f32v(c.command.target_pos.y);
    w.f32v(c.command.target_pos.z);
    w.u32v(c.command.target_id);
    w.u32v(c.command.command_id);
    w.str(c.command.blueprint_id);
    w.u32v(static_cast<u32>(c.unit_ids.size()));
    for (u32 id : c.unit_ids) w.u32v(id);
}

bool read_command(ByteReader& r, ScheduledCommand& c) {
    c = ScheduledCommand{};
    c.exec_tick = r.u32v();
    c.source = r.u32v();
    c.clear_existing = r.u8v() != 0;
    c.command.type = static_cast<CommandType>(r.u8v());
    c.command.target_pos.x = r.f32v();
    c.command.target_pos.y = r.f32v();
    c.command.target_pos.z = r.f32v();
    c.command.target_id = r.u32v();
    c.command.command_id = r.u32v();
    c.command.blueprint_id = r.str();
    const u32 n = r.u32v();
    for (u32 i = 0; i < n && r.ok(); ++i) c.unit_ids.push_back(r.u32v());
    return r.ok();
}

} // namespace osc::sim
