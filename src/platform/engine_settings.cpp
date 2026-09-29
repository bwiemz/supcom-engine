#include "platform/engine_settings.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

namespace osc::platform {

namespace fs = std::filesystem;

namespace {

constexpr const char* kFaPath = "fa_path";

// Paths are stored as UTF-8 with forward slashes, whatever the platform
std::string to_utf8(const fs::path& path) {
    const std::u8string u8 = path.generic_u8string();
    return {u8.begin(), u8.end()};
}

fs::path from_utf8(const std::string& text) {
    return {std::u8string(text.begin(), text.end())};
}

/// The file's JSON object: empty when it is missing, unreadable or not an
/// object.
nlohmann::json read_object(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return nlohmann::json::object();
    std::ostringstream text;
    text << in.rdbuf();
    auto json = nlohmann::json::parse(text.str(), nullptr, /*allow_exceptions=*/false);
    return json.is_object() ? json : nlohmann::json::object();
}

} // namespace

fs::path engine_settings_path(const EnvLookup& env) {
    return known_folder(KnownFolder::Config, env) / "opensupcom" / "settings.json";
}

fs::path engine_log_file(const EnvLookup& env) {
    return known_folder(KnownFolder::State, env) / "opensupcom" / "logs" / "opensupcom.log";
}

fs::path engine_crash_dir(const EnvLookup& env) {
    return known_folder(KnownFolder::State, env) / "opensupcom" / "crashes";
}

EngineSettings load_engine_settings(const fs::path& file) {
    EngineSettings settings;
    const auto json = read_object(file);
    if (auto it = json.find(kFaPath); it != json.end() && it->is_string()) {
        const auto& text = it->get_ref<const std::string&>();
        if (!text.empty()) settings.fa_path = from_utf8(text);
    }
    return settings;
}

bool save_engine_settings(const fs::path& file, const EngineSettings& settings) {
    auto json = read_object(file);
    if (settings.fa_path) json[kFaPath] = to_utf8(*settings.fa_path);
    else json.erase(kFaPath);

    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    fs::path temp = file;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << json.dump(2) << '\n';
        if (!out.flush()) return false;
    }
    fs::rename(temp, file, ec);
    if (ec) {
        fs::remove(temp, ec);
        return false;
    }
    return true;
}

} // namespace osc::platform
