#pragma once

#include "core/types.hpp"
#include "sim/entity.hpp" // for Vector3

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace osc::audio {

class XwbParser;
class XsbParser;

using SoundHandle = u32;
constexpr SoundHandle INVALID_SOUND = 0;

/// Manages audio playback: loads XACT3 sound banks, plays sounds via miniaudio.
/// Gracefully degrades to headless mode if no audio device or sounds directory.
class SoundManager {
public:
    explicit SoundManager(const fs::path& sounds_dir);
    ~SoundManager();

    // Non-copyable
    SoundManager(const SoundManager&) = delete;
    SoundManager& operator=(const SoundManager&) = delete;

    bool is_headless() const { return headless_; }

    /// Play a one-shot sound at the given world position.
    SoundHandle play(const std::string& bank, const std::string& cue,
                     const sim::Vector3* pos = nullptr);

    /// Play a looping sound at the given world position. Returns handle for stop().
    SoundHandle play_loop(const std::string& bank, const std::string& cue,
                          const sim::Vector3* pos = nullptr);

    /// Stop a previously started looping sound.
    void stop(SoundHandle handle);

    /// Update the listener position (call once per tick).
    void set_listener_position(const sim::Vector3& pos);

    /// Clean up finished one-shot sounds. Call periodically (e.g. per tick).
    void gc();

private:
    struct BankPair {
        std::unique_ptr<XwbParser> xwb;
        std::unique_ptr<XsbParser> xsb;
    };

    struct AudioEngine;
    struct ActiveSound;

    /// Lazy-load a bank pair by name. Returns nullptr if not found.
    BankPair* ensure_bank(const std::string& bank_name);

    /// Synthesize a WAV header around raw wave data for miniaudio decoding.
    std::vector<u8> wrap_as_wav(const struct WaveInfo& info,
                                const std::vector<u8>& raw_data);

    /// Internal play implementation.
    SoundHandle play_internal(const std::string& bank, const std::string& cue,
                              const sim::Vector3* pos, bool looping);

    fs::path sounds_dir_;
    bool headless_ = false;

    std::unique_ptr<AudioEngine> engine_;
    std::unordered_map<std::string, std::unique_ptr<BankPair>> banks_;

    u32 next_handle_ = 1;
    std::unordered_map<SoundHandle, std::unique_ptr<ActiveSound>> active_sounds_;
};

} // namespace osc::audio
