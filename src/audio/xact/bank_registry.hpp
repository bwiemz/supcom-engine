#pragma once

#include "audio/xact/global_settings.hpp"
#include "audio/xact/sound_bank.hpp"
#include "audio/xwb_parser.hpp"
#include "core/types.hpp"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace osc::audio::xact {

/// FA's sound banks in one directory (`<fa>/sounds`). A sound bank is
/// named by its file (`Sound{Bank = 'UEL'}` is `UEL.xsb`); the wave banks it
/// lists are named by their *internal* name, which need not be a file name
/// (`XAS_Weapons.xwb` is `XAS_Weapon`), and one sound bank may draw on
/// several (retail's Interface bank uses five). Names match case-blind.
class BankRegistry {
public:
    /// Index `sounds_dir`: every wave bank's header and entry table is read
    /// (not its audio), and the global settings (.xgs) are loaded.
    explicit BankRegistry(fs::path sounds_dir);
    ~BankRegistry();
    BankRegistry(const BankRegistry&) = delete;
    BankRegistry& operator=(const BankRegistry&) = delete;

    const fs::path& dir() const { return dir_; }

    /// The XACT global settings, or nullptr if the directory has none.
    const GlobalSettings* global_settings() const { return settings_ ? &*settings_ : nullptr; }

    /// The sound bank `name` (parsed on first use), or nullptr.
    const SoundBank* sound_bank(std::string_view name);

    /// The wave bank with internal name `name`, or nullptr.
    const XwbParser* wave_bank(std::string_view name) const;

    /// A wave a sound bank refers to, resolved to its wave bank.
    struct Wave {
        const XwbParser* bank = nullptr;
        u32 index = 0;
    };
    std::optional<Wave> resolve(const SoundBank& sb, const WaveChoice& w) const;

    /// File stems of the sound banks present, sorted.
    std::vector<std::string> sound_bank_names() const;

private:
    fs::path dir_;
    std::optional<GlobalSettings> settings_;
    std::unordered_map<std::string, fs::path> sound_bank_files_; ///< lower stem -> file
    std::unordered_map<std::string, std::unique_ptr<SoundBank>> sound_banks_; ///< lower stem
    std::unordered_map<std::string, std::unique_ptr<XwbParser>> wave_banks_; ///< lower name
};

} // namespace osc::audio::xact
