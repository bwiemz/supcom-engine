#pragma once

#include "core/types.hpp"

#include <cstring>
#include <span>
#include <string>

namespace osc::audio::xact {

/// Little-endian reads over a byte span, bounds-checked: a read past the
/// end yields 0 and clears ok(), so a truncated or hostile file fails its
/// parse instead of reading out of bounds.
class ByteReader {
public:
    explicit ByteReader(std::span<const u8> data, size_t pos = 0) : data_(data), pos_(pos) {
        if (pos_ > data_.size()) ok_ = false;
    }

    bool ok() const { return ok_; }
    size_t pos() const { return pos_; }
    size_t size() const { return data_.size(); }

    /// Move to an absolute offset (fails past the end).
    void seek(size_t pos) {
        if (pos > data_.size()) ok_ = false;
        else pos_ = pos;
    }
    void skip(size_t n) { seek(pos_ + n); }

    u8 read_u8() { return static_cast<u8>(take(1)); }
    u16 read_u16() { return static_cast<u16>(take(2)); }
    i16 read_s16() { return static_cast<i16>(take(2)); }
    u32 read_u32() { return static_cast<u32>(take(4)); }
    i32 read_s32() { return static_cast<i32>(take(4)); }
    f32 read_f32() {
        const u32 bits = read_u32();
        f32 v;
        std::memcpy(&v, &bits, sizeof v);
        return v;
    }

    /// The NUL-terminated string at `at` (at most `max` bytes); empty on a
    /// bad offset.
    std::string cstr(size_t at, size_t max = 256) {
        if (at >= data_.size()) {
            ok_ = false;
            return {};
        }
        const size_t limit = std::min(data_.size(), at + max);
        size_t end = at;
        while (end < limit && data_[end] != 0) ++end;
        return {reinterpret_cast<const char*>(data_.data() + at), end - at};
    }

private:
    u64 take(size_t n) {
        if (!ok_ || pos_ + n > data_.size()) {
            ok_ = false;
            return 0;
        }
        u64 v = 0;
        for (size_t i = 0; i < n; ++i) v |= static_cast<u64>(data_[pos_ + i]) << (8 * i);
        pos_ += n;
        return v;
    }

    std::span<const u8> data_;
    size_t pos_ = 0;
    bool ok_ = true;
};

} // namespace osc::audio::xact
