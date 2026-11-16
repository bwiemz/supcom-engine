#include "audio/xact/bank_registry.hpp"

#include "vfs/virtual_file_system.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>

namespace osc::audio::xact {

namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::vector<u8> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::vector<u8> to_bytes(const std::vector<char>& data) {
    return {data.begin(), data.end()};
}

/// The files `pattern` matches in the VFS folder `dir` itself, sorted, as
/// Moho's EnumerateFiles(dir, pattern, false) lists them (find_files also
/// looks in the folders below).
std::vector<std::string> files_in(const vfs::VirtualFileSystem& vfs, const std::string& dir,
                                  std::string_view pattern) {
    const std::string prefix = dir == "/" ? dir : dir + "/";
    std::vector<std::string> out;
    for (auto& path : vfs.find_files(dir, pattern)) {
        if (path.size() > prefix.size() && path.compare(0, prefix.size(), prefix) == 0 &&
            path.find('/', prefix.size()) == std::string::npos)
            out.push_back(std::move(path));
    }
    std::sort(out.begin(), out.end());
    return out;
}

/// "/sounds/uel.xsb" -> "uel"
std::string stem(std::string_view path) {
    const size_t slash = path.rfind('/');
    std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    if (const size_t dot = name.rfind('.'); dot != std::string_view::npos)
        name = name.substr(0, dot);
    return std::string(name);
}

} // namespace

BankRegistry::BankRegistry(const vfs::VirtualFileSystem& vfs, std::string_view dir) {
    const std::string xgs = vfs::VirtualFileSystem::normalize(dir) + "/SupCom.xgs";
    if (auto data = vfs.read_file(xgs)) {
        if (auto r = GlobalSettings::parse(to_bytes(*data))) {
            settings_ = std::move(r.value());
        } else {
            spdlog::warn("Audio: {}: {}", xgs, r.error().message);
        }
    }
    add_directory(vfs, dir);
}

void BankRegistry::add_directory(const vfs::VirtualFileSystem& vfs, std::string_view dir) {
    const std::string folder = vfs::VirtualFileSystem::normalize(dir);
    const size_t wave_banks = wave_banks_.size();
    for (const auto& path : files_in(vfs, folder, "*.xwb")) {
        auto wb = std::make_unique<XwbParser>();
        Result<void> r;
        if (const auto disk = vfs.disk_path(path)) {
            r = wb->parse(*disk);
        } else if (auto data = vfs.read_file(path)) {
            r = wb->parse(std::make_shared<const std::vector<u8>>(to_bytes(*data)));
            if (r && wb->streaming()) {
                spdlog::warn("Audio: Error loading WaveBank '{}': a streaming wave bank, in "
                             "an archive",
                             path);
                continue;
            }
        } else {
            r = Error("unreadable");
        }
        if (!r) {
            spdlog::warn("Audio: wave bank {}: {}", path, r.error().message);
            continue;
        }
        const std::string name = lower(wb->bank_name());
        wave_banks_.try_emplace(name, std::move(wb));
    }

    size_t sound_banks = 0;
    for (const auto& path : files_in(vfs, folder, "*.xsb")) {
        SoundBankFile file{stem(path), {}, {}};
        const std::string key = lower(file.name);
        if (sound_bank_files_.contains(key)) continue;
        if (auto disk = vfs.disk_path(path)) {
            file.disk = std::move(*disk);
        } else if (auto data = vfs.read_file(path)) {
            file.bytes = to_bytes(*data);
        } else {
            spdlog::warn("Audio: sound bank {}: unreadable", path);
            continue;
        }
        sound_bank_files_.emplace(key, std::move(file));
        ++sound_banks;
        // A miss cached before this folder came is one no longer.
        if (auto it = sound_banks_.find(key); it != sound_banks_.end() && !it->second)
            sound_banks_.erase(it);
    }
    if (sound_banks != 0) {
        spdlog::info("Audio: {} sound banks, {} wave banks, {} categories in {}", sound_banks,
                     wave_banks_.size() - wave_banks, settings_ ? settings_->categories.size() : 0,
                     folder);
    }
}

BankRegistry::~BankRegistry() = default;

const SoundBank* BankRegistry::sound_bank(std::string_view name) {
    const std::string key = lower(name);
    if (auto it = sound_banks_.find(key); it != sound_banks_.end()) return it->second.get();
    std::unique_ptr<SoundBank> bank; // stays null on a miss, cached
    if (auto file = sound_bank_files_.find(key); file != sound_bank_files_.end()) {
        const SoundBankFile& f = file->second;
        auto r = f.disk.empty() ? SoundBank::parse(f.bytes) : SoundBank::parse(read_file(f.disk));
        if (r) {
            bank = std::make_unique<SoundBank>(std::move(r.value()));
        } else {
            spdlog::warn("Audio: sound bank {}: {}", f.name, r.error().message);
        }
    }
    return sound_banks_.emplace(key, std::move(bank)).first->second.get();
}

const XwbParser* BankRegistry::wave_bank(std::string_view name) const {
    auto it = wave_banks_.find(lower(name));
    return it == wave_banks_.end() ? nullptr : it->second.get();
}

std::optional<BankRegistry::Wave> BankRegistry::resolve(const SoundBank& sb,
                                                        const WaveChoice& w) const {
    if (w.bank >= sb.wave_banks.size()) return std::nullopt;
    const XwbParser* bank = wave_bank(sb.wave_banks[w.bank]);
    if (!bank || w.wave >= bank->entry_count()) return std::nullopt;
    return Wave{bank, w.wave};
}

std::vector<std::string> BankRegistry::sound_bank_names() const {
    std::vector<std::string> names;
    names.reserve(sound_bank_files_.size());
    for (const auto& [key, file] : sound_bank_files_) names.push_back(file.name);
    std::sort(names.begin(), names.end());
    return names;
}

} // namespace osc::audio::xact
