#include "audio/xact/bank_registry.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <system_error>

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

} // namespace

BankRegistry::BankRegistry(fs::path sounds_dir) : dir_(std::move(sounds_dir)) {
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir_, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        const fs::path& p = entry.path();
        const std::string ext = lower(p.extension().string());
        if (ext == ".xsb") {
            sound_bank_files_.emplace(lower(p.stem().string()), p);
        } else if (ext == ".xwb") {
            auto wb = std::make_unique<XwbParser>();
            if (auto r = wb->parse(p); !r) {
                spdlog::warn("Audio: wave bank {}: {}", p.filename().string(), r.error().message);
                continue;
            }
            const std::string name = lower(wb->bank_name());
            wave_banks_.emplace(name, std::move(wb));
        } else if (ext == ".xgs" && !settings_) {
            const auto bytes = read_file(p);
            if (auto r = GlobalSettings::parse(bytes)) {
                settings_ = std::move(r.value());
            } else {
                spdlog::warn("Audio: {}: {}", p.filename().string(), r.error().message);
            }
        }
    }
    if (!sound_bank_files_.empty()) {
        spdlog::info("Audio: {} sound banks, {} wave banks, {} categories in {}",
                     sound_bank_files_.size(), wave_banks_.size(),
                     settings_ ? settings_->categories.size() : 0, dir_.string());
    }
}

BankRegistry::~BankRegistry() = default;

const SoundBank* BankRegistry::sound_bank(std::string_view name) {
    const std::string key = lower(name);
    if (auto it = sound_banks_.find(key); it != sound_banks_.end()) return it->second.get();
    std::unique_ptr<SoundBank> bank; // stays null on a miss, cached
    if (auto file = sound_bank_files_.find(key); file != sound_bank_files_.end()) {
        const auto bytes = read_file(file->second);
        if (auto r = SoundBank::parse(bytes)) {
            bank = std::make_unique<SoundBank>(std::move(r.value()));
        } else {
            spdlog::warn("Audio: sound bank {}: {}", file->second.filename().string(),
                         r.error().message);
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
    for (const auto& [key, path] : sound_bank_files_) names.push_back(path.stem().string());
    std::sort(names.begin(), names.end());
    return names;
}

} // namespace osc::audio::xact
