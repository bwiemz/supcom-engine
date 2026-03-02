#pragma once

#include "core/result.hpp"
#include "core/types.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace osc::audio {

/// Maps a cue name to a wave bank + track index.
struct CueMapping {
    u16 wave_bank_index = 0; ///< Index into the XSB's wave bank name list
    u16 track_index = 0;     ///< Wave entry index within that XWB
};

/// Parses XACT3 .xsb (Xbox Sound Bank) files.
/// Extracts cue_name → (wave_bank_index, track_index) mappings.
class XsbParser {
public:
    Result<void> parse(const fs::path& xsb_path);

    /// Look up a cue by name. Returns nullptr if not found.
    const CueMapping* find_cue(const std::string& name) const;

    /// Get the wave bank name by index.
    const std::string& wavebank_name(u32 index) const {
        static const std::string empty;
        return index < wavebank_names_.size() ? wavebank_names_[index] : empty;
    }
    u32 wavebank_count() const { return static_cast<u32>(wavebank_names_.size()); }
    u32 cue_count() const { return static_cast<u32>(cue_map_.size()); }

private:
    std::unordered_map<std::string, CueMapping> cue_map_;
    std::vector<std::string> wavebank_names_;
};

} // namespace osc::audio
