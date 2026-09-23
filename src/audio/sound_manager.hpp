#pragma once

#include "core/types.hpp"
#include "sim/entity.hpp" // for Vector3

#include <functional>
#include <list>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace osc::audio {

namespace xact {
class BankRegistry;
struct Cue;
struct PlayEvent;
class SoundBank;
struct Sound;
} // namespace xact

class XwbParser;

using SoundHandle = u32;
constexpr SoundHandle INVALID_SOUND = 0;

/// FA's audio: XACT cues played the way XACT plays them. It is owned by the
/// application (the front end has sound too) and shared by the UI and sim
/// Lua states.
///
/// A cue plays its sound's tracks. Each play event picks its wave by the
/// event's variation mode and effect variation, and honours its loop count.
/// The volume is:
///
///     sound + track + variation + RPC curves (all in millibels)
///       x category chain x the player's category volumes x fade
///
/// The RPC curves read per-instance variables (Distance to the listener,
/// AttackTime, ReleaseTime) and global ones (CameraDistance, ZoomPercent,
/// Duck...). Category and cue instance limits apply on play. A stop that is
/// not immediate fades out, or runs the release curve.
///
/// Without an output device (tests, headless) the same logic runs on the
/// waves' durations, so timing, limits and fades are observable.
class SoundManager {
public:
    /// `output`: open an audio device (false for tests and headless runs).
    explicit SoundManager(const fs::path& sounds_dir, bool output = true);
    ~SoundManager();

    SoundManager(const SoundManager&) = delete;
    SoundManager& operator=(const SoundManager&) = delete;

    /// No output device (sounds still run their course).
    bool is_headless() const { return !output_; }
    /// Whether FA's sound data was found.
    bool has_data() const { return registry_ != nullptr; }

    /// FA's banks in the sounds directory, or nullptr when there is none.
    xact::BankRegistry* registry() { return registry_.get(); }

    /// Play cue `cue` of sound bank `bank`: 2D, or at `pos` in the world.
    /// A LodCutoff variable (Sound{LodCutoff=...}) culls a positional
    /// sound further from the listener than the variable's value.
    /// INVALID_SOUND when the cue is unknown, culled or over its limits.
    SoundHandle play(const std::string& bank, const std::string& cue,
                     const sim::Vector3* pos = nullptr, std::string_view lod_cutoff = {});

    /// play(), looping even if the cue does not (entity ambient sounds).
    SoundHandle play_loop(const std::string& bank, const std::string& cue,
                          const sim::Vector3* pos = nullptr);

    /// Stop a sound. Not immediate: fade out (the cue's or its category's
    /// fade) or run the sound's release curve; with neither, stop now.
    void stop(SoundHandle handle, bool immediate = true);
    /// Stop everything, at once.
    void stop_all();

    /// Whether the sound is still playing (a fading or releasing sound is).
    bool is_playing(SoundHandle handle) const;
    /// Call `fn` once when the sound ends (at once if it already has).
    void on_finished(SoundHandle handle, std::function<void()> fn);

    /// Move a positional sound (an entity's ambient loop follows it).
    void set_position(SoundHandle handle, const sim::Vector3& pos);

    /// The listener: the camera.
    void set_listener(const sim::Vector3& pos, const sim::Vector3& forward);
    /// Backwards-compatible: position only.
    void set_listener_position(const sim::Vector3& pos);

    /// A global XACT variable (CameraDistance, ZoomPercent, Angle, Duck...);
    /// unknown names are ignored.
    void set_global_variable(std::string_view name, f32 value);
    f32 global_variable(std::string_view name) const;

    /// The player's volume for a category (FA's options: Global, World,
    /// Interface, Music, VO), 0..1; it applies to the category's subtree.
    void set_category_volume(std::string_view category, f32 volume);
    /// 1 for an unknown category.
    f32 category_volume(std::string_view category) const;

    /// Mute or restore the World category (retail does it for movies).
    void set_world_enabled(bool enabled);

    /// Advance by `dt` seconds: fire track events, end finished waves, run
    /// fades, update volumes and positions, retire finished sounds.
    void update(f32 dt);
    /// Backwards-compatible: update(0) (retires finished sounds).
    void gc() { update(0.0f); }

    /// Whether the sim tick advances the sound clock (headless runs have no
    /// frames). A windowed game advances it per frame instead.
    void set_sim_clocked(bool v) { sim_clocked_ = v; }
    bool sim_clocked() const { return sim_clocked_; }

    /// Sounds playing (fading ones included), for tests and profiling.
    size_t active_count() const { return instances_.size(); }
    /// A playing sound's loudest voice gain as last applied (0..~2), or 0.
    f32 current_gain(SoundHandle handle) const;
    /// Whether an instance of `cue` in `bank` is playing.
    bool is_cue_playing(std::string_view bank, std::string_view cue) const;

private:
    struct AudioEngine;
    struct Voice;
    struct CueInstance;
    struct WaveData;

    /// Decoded-ready (WAV-wrapped) data of a wave, cached.
    std::shared_ptr<const WaveData> wave_data(const XwbParser& bank, u32 index);
    void start_event(CueInstance& inst, size_t track, const xact::PlayEvent& ev);
    void end_instance(CueInstance& inst);
    bool admit(const xact::SoundBank& sb, const xact::Cue& cue, const xact::Sound& sound);
    int category_of(const CueInstance& inst) const;
    f32 category_gain(int category) const;
    f32 distance(const CueInstance& inst) const;
    f32 volume_mb(const CueInstance& inst, size_t track) const;
    f32 pitch_cents(const CueInstance& inst, size_t track) const;
    void apply(CueInstance& inst);
    u32 pick_wave(const xact::PlayEvent& ev);

    fs::path sounds_dir_;
    bool output_ = false;
    bool sim_clocked_ = false;
    bool world_enabled_ = true;

    std::unique_ptr<AudioEngine> engine_;
    std::unique_ptr<xact::BankRegistry> registry_;
    std::mt19937 rng_{std::random_device{}()}; // variation: output only, never sim state

    sim::Vector3 listener_{};
    sim::Vector3 listener_forward_{0, 0, 1};
    std::vector<f32> globals_;         ///< per XGS variable (global ones)
    std::vector<f32> user_volume_;     ///< per category, the player's 0..1
    int world_category_ = -1;
    int release_variable_ = -1, attack_variable_ = -1, distance_variable_ = -1;
    int cue_instances_variable_ = -1;

    struct VariationState {
        u32 last = 0xFFFFFFFF;
        std::vector<u32> order; ///< shuffle order, consumed from the back
    };
    std::unordered_map<const xact::PlayEvent*, VariationState> variation_;

    struct WaveKey {
        const XwbParser* bank;
        u32 index;
        bool operator==(const WaveKey& o) const { return bank == o.bank && index == o.index; }
    };
    struct WaveKeyHash {
        size_t operator()(const WaveKey& k) const {
            return std::hash<const void*>()(k.bank) ^ (static_cast<size_t>(k.index) * 0x9E3779B9u);
        }
    };
    std::unordered_map<WaveKey, std::shared_ptr<const WaveData>, WaveKeyHash> waves_;
    std::list<WaveKey> wave_lru_; ///< most recent at the front
    size_t wave_bytes_ = 0;

    u32 next_handle_ = 1;
    f64 clock_ = 0; ///< seconds since start
    std::unordered_map<SoundHandle, std::unique_ptr<CueInstance>> instances_;
};

} // namespace osc::audio
