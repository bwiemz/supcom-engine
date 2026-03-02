#pragma once

#include "core/result.hpp"
#include "core/types.hpp"

#include <string>
#include <vector>

namespace osc::audio {

/// Decoded metadata for a single wave entry in an XWB wave bank.
struct WaveInfo {
    u32 format_tag = 0;     ///< 0=PCM, 2=ADPCM
    u32 channels = 1;
    u32 sample_rate = 22050;
    u32 block_align = 0;
    u32 bits_per_sample = 16;
    u32 data_offset = 0;    ///< Absolute file offset of wave data
    u32 data_length = 0;    ///< Byte count of wave data
    u32 duration_samples = 0;
};

/// Parses XACT3 .xwb (Xbox Wave Bank) files.
/// Only reads metadata on parse(); wave data is lazily read per-entry.
class XwbParser {
public:
    /// Parse wave bank header and entry metadata from file.
    Result<void> parse(const fs::path& xwb_path);

    const std::string& bank_name() const { return bank_name_; }
    u32 entry_count() const { return static_cast<u32>(entries_.size()); }
    const WaveInfo& entry(u32 index) const { return entries_[index]; }

    /// Read raw wave data for a specific entry from the file (lazy I/O).
    std::vector<u8> read_wave_data(u32 index) const;

private:
    fs::path path_;
    std::string bank_name_;
    std::vector<WaveInfo> entries_;
};

} // namespace osc::audio
