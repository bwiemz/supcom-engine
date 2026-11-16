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

namespace osc::vfs {
class VirtualFileSystem;
}

namespace osc::audio::xact {

/// FA's sound banks, as the VFS holds them: Moho's audio engines list them
/// in `/sounds` (and the voice folders), so a mod's or a map's sounds
/// folder mounted there by the init file joins FA's own. A sound bank is
/// named by its file (`Sound{Bank = 'UEL'}` is `/sounds/uel.xsb`); the wave
/// banks it lists are named by their *internal* name, which need not be a
/// file name (`XAS_Weapons.xwb` is `XAS_Weapon`), and one sound bank may
/// draw on several (retail's Interface bank uses five). Names match
/// case-blind.
class BankRegistry {
public:
    /// The global settings Moho's func_InitSound reads, `<dir>/SupCom.xgs`,
    /// and the banks in `dir` (Moho's is "/sounds"). Every wave bank's
    /// header and entry table is read (not its audio).
    BankRegistry(const vfs::VirtualFileSystem& vfs, std::string_view dir);
    ~BankRegistry();
    BankRegistry(const BankRegistry&) = delete;
    BankRegistry& operator=(const BankRegistry&) = delete;

    /// Index the banks the VFS holds in `dir`, as Moho's func_LoadSoundPath
    /// (0x004DA500) loads an engine's: the `*.xwb`, then the `*.xsb`, in
    /// that folder alone (not its subfolders), from every mount. A file two
    /// mounts hold is the first mount's, as any path resolves; a bank name
    /// already known keeps its first bank (FA's voice banks sit in
    /// /sounds/voice/<language>, indexed after /sounds).
    ///
    /// A bank in a folder mount is read from its file. One in an archive is
    /// read into memory; but a streaming wave bank there is skipped with a
    /// warning, as Moho opens a streaming bank as a file, which fails inside
    /// an archive.
    void add_directory(const vfs::VirtualFileSystem& vfs, std::string_view dir);

    /// The XACT global settings, or nullptr if there are none.
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

    /// The sound banks present, by their names (the files' stems, lower
    /// case as the VFS lists them), sorted.
    std::vector<std::string> sound_bank_names() const;

private:
    /// A sound bank's file: read from disk on first use, or (one in an
    /// archive) its bytes, read when it was listed.
    struct SoundBankFile {
        std::string name;
        fs::path disk;
        std::vector<u8> bytes;
    };

    std::optional<GlobalSettings> settings_;
    std::unordered_map<std::string, SoundBankFile> sound_bank_files_;         ///< lower stem
    std::unordered_map<std::string, std::unique_ptr<SoundBank>> sound_banks_; ///< lower stem
    std::unordered_map<std::string, std::unique_ptr<XwbParser>> wave_banks_; ///< lower name
};

} // namespace osc::audio::xact
