#pragma once

#include "core/types.hpp"
#include "sim/command_scheduler.hpp"

#include <string>
#include <vector>

namespace osc::sim {

/// Little-endian byte encoding shared by the replay file and the lockstep
/// wire, so a command reads back the same wherever it was written.
class ByteWriter {
public:
    explicit ByteWriter(std::vector<u8>& out) : b_(out) {}
    void u8v(u8 v) { b_.push_back(v); }
    void u32v(u32 v);
    void u64v(u64 v);
    void f32v(f32 v);
    void f64v(f64 v);
    void str(const std::string& s);

private:
    std::vector<u8>& b_;
};

/// Reads what ByteWriter wrote; a read past the end sets ok() false and
/// yields zeros from then on.
class ByteReader {
public:
    explicit ByteReader(const std::vector<u8>& in) : b_(in) {}
    u8 u8v();
    u32 u32v();
    u64 u64v();
    f32 f32v();
    f64 f64v();
    std::string str();
    bool ok() const { return ok_; }
    size_t position() const { return pos_; }

private:
    bool need(size_t n);
    const std::vector<u8>& b_;
    size_t pos_ = 0;
    bool ok_ = true;
};

void write_command(ByteWriter& w, const ScheduledCommand& c);
/// False when the bytes ran out.
bool read_command(ByteReader& r, ScheduledCommand& c);

} // namespace osc::sim
