#include "audio/xwb_parser.hpp"

#include <spdlog/spdlog.h>

#include <cstring>
#include <fstream>

namespace osc::audio {

// XACT3 XWB format constants
static constexpr char XWB_MAGIC[4] = {'W', 'B', 'N', 'D'};
static constexpr u32 XWB_MIN_VERSION = 42;

// Segment indices
static constexpr u32 SEG_BANKDATA = 0;
static constexpr u32 SEG_ENTRYMETADATA = 1;
// static constexpr u32 SEG_SEEKTABLES = 2;
// static constexpr u32 SEG_ENTRYNAMES = 3;
static constexpr u32 SEG_ENTRYWAVEDATA = 4;

// BANKDATA flags
static constexpr u32 BANKDATA_FLAGS_COMPACT = 0x00020000;

// MINIWAVEFORMAT codec tags
static constexpr u32 CODEC_PCM = 0;
// static constexpr u32 CODEC_XMA = 1;
static constexpr u32 CODEC_ADPCM = 2;
// static constexpr u32 CODEC_WMA = 3;

/// Read a little-endian u32 from a buffer.
static u32 read_u32(const u8* p) {
    return static_cast<u32>(p[0]) |
           (static_cast<u32>(p[1]) << 8) |
           (static_cast<u32>(p[2]) << 16) |
           (static_cast<u32>(p[3]) << 24);
}

/// Decode XACT3 MINIWAVEFORMAT packed bitfield.
static WaveInfo decode_miniwaveformat(u32 fmt) {
    WaveInfo info;
    info.format_tag     = fmt & 0x3;           // bits 0-1
    info.channels       = (fmt >> 2) & 0x7;    // bits 2-4
    info.sample_rate    = (fmt >> 5) & 0x3FFFF; // bits 5-22
    u32 raw_block_align = (fmt >> 23) & 0xFF;  // bits 23-30
    u32 bits_flag       = (fmt >> 31) & 0x1;   // bit 31

    info.bits_per_sample = bits_flag ? 16 : 8;

    // Block align interpretation depends on codec
    if (info.format_tag == CODEC_PCM) {
        info.block_align = info.channels * info.bits_per_sample / 8;
    } else if (info.format_tag == CODEC_ADPCM) {
        info.block_align = (raw_block_align + 22) * info.channels;
    } else {
        info.block_align = raw_block_align;
    }

    return info;
}

Result<void> XwbParser::parse(const fs::path& xwb_path) {
    path_ = xwb_path;
    entries_.clear();
    bank_name_.clear();

    std::ifstream file(xwb_path, std::ios::binary);
    if (!file) {
        return Error("Failed to open XWB file: " + xwb_path.string());
    }

    // Read header: 4 magic + 4 version + 4 header_version + 5*8 segments = 52 bytes
    u8 header[52];
    file.read(reinterpret_cast<char*>(header), 52);
    if (!file) {
        return Error("Failed to read XWB header");
    }

    // Validate magic
    if (std::memcmp(header, XWB_MAGIC, 4) != 0) {
        return Error("Invalid XWB magic");
    }

    u32 version = read_u32(header + 4);
    if (version < XWB_MIN_VERSION) {
        return Error("Unsupported XWB version: " + std::to_string(version));
    }

    // Parse 5 segment descriptors (offset, length pairs)
    struct Segment { u32 offset, length; };
    Segment segments[5];
    for (int i = 0; i < 5; i++) {
        segments[i].offset = read_u32(header + 12 + i * 8);
        segments[i].length = read_u32(header + 12 + i * 8 + 4);
    }

    // Read BANKDATA segment (96 bytes for non-compact)
    auto& bd_seg = segments[SEG_BANKDATA];
    if (bd_seg.length < 24) { // minimum: flags + count + partial name
        return Error("BANKDATA segment too small");
    }

    std::vector<u8> bankdata(bd_seg.length);
    file.seekg(bd_seg.offset);
    file.read(reinterpret_cast<char*>(bankdata.data()), bd_seg.length);
    if (!file) {
        return Error("Failed to read BANKDATA");
    }

    u32 flags = read_u32(bankdata.data());
    u32 entry_count = read_u32(bankdata.data() + 4);
    bool is_compact = (flags & BANKDATA_FLAGS_COMPACT) != 0;

    // Bank name: 64 bytes starting at offset 8
    bank_name_.assign(reinterpret_cast<const char*>(bankdata.data() + 8), 64);
    auto null_pos = bank_name_.find('\0');
    if (null_pos != std::string::npos) bank_name_.resize(null_pos);

    u32 entry_meta_size = 0;
    u32 compact_format = 0;
    if (bd_seg.length >= 80) {
        entry_meta_size = read_u32(bankdata.data() + 72); // offset 72 in bankdata
    }
    if (bd_seg.length >= 88) {
        compact_format = read_u32(bankdata.data() + 84); // alignment at 80, compact fmt at 84
    }

    // Wave data base offset
    auto& wd_seg = segments[SEG_ENTRYWAVEDATA];

    // Read entry metadata
    auto& em_seg = segments[SEG_ENTRYMETADATA];
    if (em_seg.length == 0) {
        spdlog::warn("XWB {}: no entry metadata segment", bank_name_);
        return Result<void>();
    }

    std::vector<u8> entry_meta(em_seg.length);
    file.seekg(em_seg.offset);
    file.read(reinterpret_cast<char*>(entry_meta.data()), em_seg.length);
    if (!file) {
        return Error("Failed to read entry metadata");
    }

    entries_.resize(entry_count);

    if (is_compact) {
        // Compact format: all entries share the same MINIWAVEFORMAT from bankdata
        WaveInfo shared = decode_miniwaveformat(compact_format);
        u32 alignment = (bd_seg.length >= 84) ? read_u32(bankdata.data() + 80) : 1;
        if (alignment == 0) alignment = 1;

        for (u32 i = 0; i < entry_count; i++) {
            if (i * 4 + 4 > em_seg.length) break;

            u32 packed = read_u32(entry_meta.data() + i * 4);
            // Compact entry: bits 0-20 = offset_div_alignment, bits 21-31 = length_deviation
            u32 offset_div = packed & 0x1FFFFF;
            // i32 len_dev = static_cast<i32>(packed >> 21);

            entries_[i] = shared;
            entries_[i].data_offset = wd_seg.offset + offset_div * alignment;

            // For compact, length must be computed from next entry's offset
            if (i + 1 < entry_count) {
                u32 next_packed = read_u32(entry_meta.data() + (i + 1) * 4);
                u32 next_offset = (next_packed & 0x1FFFFF) * alignment;
                entries_[i].data_length = next_offset - offset_div * alignment;
            } else {
                entries_[i].data_length = wd_seg.length - offset_div * alignment;
            }
        }
    } else {
        // Non-compact: 24 bytes per entry
        u32 stride = entry_meta_size ? entry_meta_size : 24;
        for (u32 i = 0; i < entry_count; i++) {
            u32 base = i * stride;
            if (base + 24 > em_seg.length) break;

            const u8* p = entry_meta.data() + base;
            u32 flags_dur = read_u32(p);
            u32 format = read_u32(p + 4);
            u32 play_offset = read_u32(p + 8);
            u32 play_length = read_u32(p + 12);

            entries_[i] = decode_miniwaveformat(format);
            entries_[i].data_offset = wd_seg.offset + play_offset;
            entries_[i].data_length = play_length;
            entries_[i].duration_samples = flags_dur >> 4;
        }
    }

    spdlog::debug("XWB parsed: {} ({} entries, {})",
                  bank_name_, entry_count, is_compact ? "compact" : "non-compact");

    return Result<void>();
}

std::vector<u8> XwbParser::read_wave_data(u32 index) const {
    if (index >= entries_.size()) return {};

    const auto& e = entries_[index];
    if (e.data_length == 0) return {};

    std::ifstream file(path_, std::ios::binary);
    if (!file) return {};

    file.seekg(e.data_offset);
    std::vector<u8> data(e.data_length);
    file.read(reinterpret_cast<char*>(data.data()), e.data_length);
    if (!file) return {};

    return data;
}

} // namespace osc::audio
