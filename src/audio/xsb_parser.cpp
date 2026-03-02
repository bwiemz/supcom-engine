#include "audio/xsb_parser.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <fstream>

namespace osc::audio {

static constexpr char XSB_MAGIC[4] = {'S', 'D', 'B', 'K'};
static constexpr u16 XSB_MIN_VERSION = 43;

/// Read little-endian values from a buffer.
static u16 read_u16(const u8* p) {
    return static_cast<u16>(p[0]) | (static_cast<u16>(p[1]) << 8);
}
static u32 read_u32(const u8* p) {
    return static_cast<u32>(p[0]) |
           (static_cast<u32>(p[1]) << 8) |
           (static_cast<u32>(p[2]) << 16) |
           (static_cast<u32>(p[3]) << 24);
}

Result<void> XsbParser::parse(const fs::path& xsb_path) {
    cue_map_.clear();
    wavebank_names_.clear();

    std::ifstream file(xsb_path, std::ios::binary);
    if (!file) {
        return Error("Failed to open XSB file: " + xsb_path.string());
    }

    // Get file size
    file.seekg(0, std::ios::end);
    auto file_size = static_cast<u32>(file.tellg());
    file.seekg(0);

    // Read entire file into memory (XSB files are small, ~1-12KB)
    std::vector<u8> data(file_size);
    file.read(reinterpret_cast<char*>(data.data()), file_size);
    if (!file) {
        return Error("Failed to read XSB file");
    }

    const u8* d = data.data();

    // Validate header
    if (file_size < 0x4A + 64) {
        return Error("XSB file too small");
    }
    if (std::memcmp(d, XSB_MAGIC, 4) != 0) {
        return Error("Invalid XSB magic");
    }

    u16 tool_version = read_u16(d + 0x04);
    if (tool_version < XSB_MIN_VERSION) {
        return Error("Unsupported XSB version: " + std::to_string(tool_version));
    }

    // Parse header fields (packed struct, no alignment padding)
    // u16 crc              = read_u16(d + 0x08);
    // u32 last_mod_low     = read_u32(d + 0x0A);
    // u32 last_mod_high    = read_u32(d + 0x0E);
    // u8  platform         = d[0x12];
    u16 simple_cue_count    = read_u16(d + 0x13);
    u16 complex_cue_count   = read_u16(d + 0x15);
    // u16 unk1             = read_u16(d + 0x17);
    u16 total_cue_count     = read_u16(d + 0x19);
    u8  wavebank_count      = d[0x1B];
    // u16 sound_count      = read_u16(d + 0x1C);
    // u16 cue_name_len     = read_u16(d + 0x1E);
    // u16 unk2             = read_u16(d + 0x20);
    u32 simple_cues_off     = read_u32(d + 0x22);
    u32 complex_cues_off    = read_u32(d + 0x26);
    u32 cue_names_off       = read_u32(d + 0x2A);
    // u32 unk3             = read_u32(d + 0x2E);
    // u32 variation_off    = read_u32(d + 0x32);
    // u32 unk4             = read_u32(d + 0x36);
    u32 wavebank_names_off  = read_u32(d + 0x3A);
    // u32 hash_table_off   = read_u32(d + 0x3E);
    // u32 hash_values_off  = read_u32(d + 0x42);
    // u32 sounds_off       = read_u32(d + 0x46);
    // char bank_name[64]   @ 0x4A

    // Read wave bank names (64 bytes each)
    wavebank_names_.resize(wavebank_count);
    for (u32 i = 0; i < wavebank_count; i++) {
        u32 off = wavebank_names_off + i * 64;
        if (off + 64 > file_size) break;
        std::string name(reinterpret_cast<const char*>(d + off), 64);
        auto null_pos = name.find('\0');
        if (null_pos != std::string::npos) name.resize(null_pos);
        wavebank_names_[i] = std::move(name);
    }

    // Read cue names sequentially from the name table
    // Names are null-terminated, stored in cue index order
    std::vector<std::string> cue_names(total_cue_count);

    if (cue_names_off != 0xFFFFFFFF && cue_names_off < file_size) {
        u32 pos = cue_names_off;
        for (u16 i = 0; i < total_cue_count && pos < file_size; i++) {
            std::string name;
            while (pos < file_size && d[pos] != 0) {
                name += static_cast<char>(d[pos++]);
            }
            pos++; // skip null terminator
            cue_names[i] = std::move(name);
        }
    }

    // Helper: resolve a sound entry at an absolute file offset to a CueMapping.
    // XACT3 complex sounds have variable internal structure (tracks, RPC/DSP
    // sections, events) that differs based on flags and event types.  Rather
    // than fully parsing the complex binary layout, we exploit the fact that
    // every PlayWave event is preceded by a 0xFF separator byte in its event
    // header.  The track_index (u16 LE) lives at separator+2 and the
    // wave_bank_index (u8) at separator+4.  This works reliably for XACT3
    // version 43 used by Supreme Commander: Forged Alliance.
    auto resolve_sound = [&](u32 target) -> std::pair<CueMapping, bool> {
        CueMapping m;
        if (target + 12 > file_size) return {m, false};

        u8 snd_flags = d[target];
        bool is_complex_sound = (snd_flags & 0x01) != 0;

        if (!is_complex_sound) {
            // Simple sound: 9-byte header + track_index(u16) + wave_bank_index(u8)
            u32 track_off = target + 9;
            if (track_off + 3 <= file_size) {
                m.track_index = read_u16(d + track_off);
                m.wave_bank_index = d[track_off + 2];
                if (m.wave_bank_index < wavebank_count) return {m, true};
            }
        } else {
            // Complex sound: entry_length at +7 gives total entry size.
            // Scan for first 0xFF separator after the track headers (+18).
            u16 entry_len = read_u16(d + target + 7);
            u32 entry_end = target + entry_len;
            if (entry_end > file_size) entry_end = file_size;

            // Start scanning after track headers (9 common + 1 numTracks + 8 track = 18)
            for (u32 p = target + 18; p + 5 <= entry_end; p++) {
                if (d[p] == 0xFF) {
                    // Separator found — track_index at p+2, wave_bank at p+4
                    m.track_index = read_u16(d + p + 2);
                    m.wave_bank_index = d[p + 4];
                    if (m.wave_bank_index < wavebank_count) return {m, true};
                }
            }
        }
        return {m, false};
    };

    // Map simple cues (each 4 bytes: u8 flags + u24 sound_offset)
    if (simple_cues_off != 0xFFFFFFFF && simple_cues_off < file_size) {
        for (u16 i = 0; i < simple_cue_count; i++) {
            u32 off = simple_cues_off + i * 4;
            if (off + 4 > file_size) break;

            u8 cue_flags = d[off];
            if (cue_flags & 0x04) {
                u32 snd_off = read_u32(d + off) >> 8; // sound offset (absolute)
                auto [m, ok] = resolve_sound(snd_off);
                if (ok && i < cue_names.size() && !cue_names[i].empty()) {
                    cue_map_[cue_names[i]] = m;
                }
            }
        }
    }

    // Map complex cues (each 15 bytes: u8 flags + u32 sound_offset + 10 bytes)
    if (complex_cues_off != 0xFFFFFFFF && complex_cues_off < file_size) {
        for (u16 i = 0; i < complex_cue_count; i++) {
            u16 cue_idx = simple_cue_count + i;
            u32 off = complex_cues_off + i * 15;
            if (off + 5 > file_size) break;

            u8 cue_flags = d[off];
            if (cue_flags & 0x04) {
                // Sound offset is ABSOLUTE file offset
                u32 snd_off = read_u32(d + off + 1);
                auto [m, ok] = resolve_sound(snd_off);
                if (ok && cue_idx < cue_names.size() &&
                    !cue_names[cue_idx].empty()) {
                    cue_map_[cue_names[cue_idx]] = m;
                } else if (cue_idx < cue_names.size() &&
                           !cue_names[cue_idx].empty()) {
                    spdlog::debug("XSB: could not resolve cue '{}' (idx {})",
                                  cue_names[cue_idx], cue_idx);
                }
            }
        }
    }

    spdlog::debug("XSB parsed: {} cues mapped (simple={}, complex={}, total={})",
                  cue_map_.size(), simple_cue_count, complex_cue_count, total_cue_count);

    return Result<void>();
}

const CueMapping* XsbParser::find_cue(const std::string& name) const {
    auto it = cue_map_.find(name);
    return (it != cue_map_.end()) ? &it->second : nullptr;
}

} // namespace osc::audio
