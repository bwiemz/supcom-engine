#pragma once

#include "audio/xact/global_settings.hpp" // LimitBehavior
#include "core/result.hpp"
#include "core/types.hpp"

#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace osc::audio::xact {

/// How a play event with several waves picks the next one.
enum class VariationMode : u8 {
    Ordered = 0,
    OrderedFromRandom = 1,
    Random = 2,
    RandomNoImmediateRepeat = 3,
    Shuffle = 4,
};

/// A wave a play event can start: index `wave` in the sound bank's wave
/// bank number `bank`, with its weight range for random choice.
struct WaveChoice {
    u16 wave = 0;
    u8 bank = 0;
    u8 weight_min = 0;
    u8 weight_max = 255;
};

/// An XACT PlayWave track event (plain, or with track and/or effect
/// variation).
struct PlayEvent {
    static constexpr u8 kLoopForever = 255;

    u32 time_ms = 0;          ///< when it fires, from the sound's start
    u16 random_offset_ms = 0; ///< plus up to this much at random
    u8 loop_count = 0;        ///< extra plays; kLoopForever loops until stopped
    std::vector<WaveChoice> waves; ///< one without track variation
    VariationMode variation = VariationMode::Ordered;
    bool vary_pitch = false;  ///< pick pitch in [pitch_min, pitch_max] cents
    bool vary_volume = false; ///< pick volume in [volume_min_mb, volume_max_mb]
    i16 pitch_min = 0, pitch_max = 0;
    f32 volume_min_mb = 0, volume_max_mb = 0;
};

struct Track {
    f32 volume_mb = 0;
    std::vector<u32> rpc_codes;
    std::vector<PlayEvent> plays;
};

struct Sound {
    u16 category = 0; ///< index into the global settings' categories
    f32 volume_mb = 0;
    i16 pitch = 0; ///< cents
    u8 priority = 0;
    std::vector<u32> rpc_codes; ///< RPC curves (GlobalSettings::rpc)
    std::vector<Track> tracks;
};

struct Cue {
    std::string name;
    u32 sound = 0; ///< index into SoundBank::sounds
    u8 instance_limit = 0xFF; ///< 0xFF: unlimited
    LimitBehavior limit_behavior = LimitBehavior::Fail;
    u16 fade_in_ms = 0;
    u16 fade_out_ms = 0;
};

/// An XACT sound bank (.xsb): named cues, each playing one sound whose
/// tracks start waves from the listed wave banks.
///
/// Parses XACT 3.0 content (version 43, as FA ships) and later; 3.0 keeps a
/// sound's events inside its entry and has a 7-byte effect variation.
class SoundBank {
public:
    std::string name;
    std::vector<std::string> wave_banks; ///< wave bank names, by index
    std::vector<Sound> sounds;
    std::vector<Cue> cues;

    /// The cue named `name`, or nullptr.
    const Cue* find_cue(std::string_view name) const;

    static Result<SoundBank> parse(std::span<const u8> data);

private:
    std::unordered_map<std::string, size_t> cue_index_;
};

} // namespace osc::audio::xact
