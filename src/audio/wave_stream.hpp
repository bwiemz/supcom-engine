#pragma once

// WAV files for miniaudio's decoder: built in memory for a short wave, or
// streamed for a long one (M216b) -- a header in memory and the samples read
// from their wave bank as they play, byte for byte the same file.

#include "core/types.hpp"

#include <fstream>
#include <vector>

namespace osc::audio {

struct WaveInfo;

/// A WAV file's header for `info`'s format (PCM, or MS-ADPCM with its
/// coefficient table) and `data_length` bytes of samples.
std::vector<u8> wav_header(const WaveInfo& info, u32 data_length);
/// A WAV file in memory: the header, then `raw`.
std::vector<u8> build_wav(const WaveInfo& info, const std::vector<u8>& raw);

/// A WAV file read on demand: `header`, then `length` bytes of `file` from
/// `offset`. Seeks clamp to the file, as miniaudio's memory decoder does.
class WaveStream {
public:
    WaveStream(std::vector<u8> header, const fs::path& file, u64 offset, u64 length);
    bool ok() const { return file_.is_open(); }
    u64 size() const { return header_.size() + length_; }
    u64 tell() const { return pos_; }
    /// Read up to `bytes` at the cursor; the count read (0 at the end, or
    /// short where the file ends early).
    size_t read(void* out, size_t bytes);
    enum class Origin { Start, Current, End };
    /// Move the cursor; from the end, `offset` is a distance back.
    void seek(i64 offset, Origin origin);

private:
    std::vector<u8> header_;
    std::ifstream file_;
    u64 offset_ = 0;
    u64 length_ = 0;
    u64 pos_ = 0;
};

} // namespace osc::audio
