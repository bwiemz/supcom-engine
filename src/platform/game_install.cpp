#include "platform/game_install.hpp"

#include "platform/steam_library.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace osc::platform {

namespace fs = std::filesystem;

namespace {

std::optional<std::string> read_text(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/// `dir/name` with the on-disk spelling of `name` (FA's files were authored
/// on Windows, e.g. "bin/SupComDataPath.lua" may appear in any case).
std::optional<fs::path> find_file_ci(const fs::path& dir, std::string_view name) {
    std::error_code ec;
    if (fs::is_regular_file(dir / name, ec)) return dir / name;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end;
         it.increment(ec)) {
        std::string entry = it->path().filename().string();
        if (entry.size() == name.size() &&
            std::equal(entry.begin(), entry.end(), name.begin(), [](char a, char b) {
                return std::tolower(static_cast<unsigned char>(a)) ==
                       std::tolower(static_cast<unsigned char>(b));
            })) {
            return it->path();
        }
    }
    return std::nullopt;
}

std::optional<fs::path> retail_init(const fs::path& fa_path) {
    return find_file_ci(fa_path / "bin", "SupComDataPath.lua");
}

fs::path default_faf_dir(const EnvLookup& env) {
#ifdef _WIN32
    auto program_data = env("ProgramData");
    return fs::path(program_data ? *program_data : "C:/ProgramData") / "FAForever";
#else
    return home_dir(env) / ".faforever";
#endif
}

std::optional<fs::path> steam_fa(const EnvLookup& env, GameInstallSearch& search) {
    auto roots = steam_roots(env);
    if (roots.empty()) search.searched.push_back("steam: no Steam installation found");
    for (const auto& root : roots) {
        search.searched.push_back("steam: " + root.string());
    }
    return find_steam_app(kForgedAllianceAppId, roots);
}

/// FA path for a FAF data dir: its fa_path.lua, else Steam.
std::optional<fs::path> faf_fa_path(const fs::path& faf_dir, const EnvLookup& env,
                                    GameInstallSearch& search) {
    if (auto text = read_text(faf_dir / "fa_path.lua")) {
        if (auto p = parse_fa_path_lua(*text)) return p;
    }
    return steam_fa(env, search);
}

/// Build an install from explicit locations (CLI or env).
std::optional<GameInstall> from_explicit(const GameInstallHints& h,
                                         const std::string& source,
                                         const EnvLookup& env,
                                         GameInstallSearch& search) {
    GameInstall install;
    install.source = source;
    install.faf_data_path = h.faf_data_path.value_or(fs::path());
    if (h.init_file) {
        install.init_file = *h.init_file;
        if (h.fa_path) {
            install.fa_path = *h.fa_path;
        } else if (!install.faf_data_path.empty()) {
            install.fa_path = faf_fa_path(install.faf_data_path, env, search)
                                  .value_or(fs::path());
        } else {
            // Retail layout: <fa>/bin/SupComDataPath.lua
            install.fa_path = install.init_file.parent_path().parent_path();
        }
        return install;
    }
    if (h.fa_path) {
        install.fa_path = *h.fa_path;
        search.searched.push_back(source + ": " + (*h.fa_path / "bin").string());
        auto init = retail_init(*h.fa_path);
        if (!init) return std::nullopt;
        install.init_file = *init;
        return install;
    }
    if (h.faf_data_path) {
        auto init = find_file_ci(*h.faf_data_path / "bin", "init_faf.lua");
        search.searched.push_back(source + ": " + h.faf_data_path->string());
        if (!init) return std::nullopt;
        install.init_file = *init;
        install.fa_path = faf_fa_path(*h.faf_data_path, env, search).value_or(fs::path());
        return install;
    }
    return std::nullopt;
}

} // namespace

std::optional<fs::path> parse_fa_path_lua(std::string_view text) {
    auto key = text.find("fa_path");
    if (key == std::string_view::npos) return std::nullopt;
    auto q1 = text.find('"', key);
    if (q1 == std::string_view::npos) return std::nullopt;
    auto q2 = text.find('"', q1 + 1);
    if (q2 == std::string_view::npos) return std::nullopt;
    std::string raw(text.substr(q1 + 1, q2 - q1 - 1));
    std::string clean;
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\\') {
            if (i + 1 < raw.size() && raw[i + 1] == '\\') ++i; // escaped '\\'
            clean += '/';
        } else {
            clean += raw[i];
        }
    }
    return fs::path(clean);
}

GameInstallSearch locate_game_install(const GameInstallHints& hints,
                                      const EnvLookup& env) {
    GameInstallSearch search;

    // 1. Command line.
    if (hints.fa_path || hints.init_file || hints.faf_data_path) {
        search.install = from_explicit(hints, "cli", env, search);
        return search;
    }

    // 2. Environment.
    GameInstallHints env_hints;
    if (auto v = env("OSC_FA_PATH"); v && !v->empty()) env_hints.fa_path = *v;
    if (auto v = env("OSC_INIT_FILE"); v && !v->empty()) env_hints.init_file = *v;
    if (auto v = env("OSC_FAF_DATA"); v && !v->empty()) env_hints.faf_data_path = *v;
    if (env_hints.fa_path || env_hints.init_file || env_hints.faf_data_path) {
        search.install = from_explicit(env_hints, "env", env, search);
        return search;
    }

    // 3. FAForever.
    const fs::path faf_dir = default_faf_dir(env);
    search.searched.push_back("faf: " + faf_dir.string());
    if (auto init = find_file_ci(faf_dir / "bin", "init_faf.lua")) {
        GameInstall install;
        install.source = "faf";
        install.init_file = *init;
        install.faf_data_path = faf_dir;
        install.fa_path = faf_fa_path(faf_dir, env, search).value_or(fs::path());
        search.install = std::move(install);
        return search;
    }

    // 4. Steam retail.
    if (auto fa = steam_fa(env, search)) {
        if (auto init = retail_init(*fa)) {
            GameInstall install;
            install.source = "steam";
            install.fa_path = *fa;
            install.init_file = *init;
            search.install = std::move(install);
        } else {
            search.searched.push_back("steam: " + fa->string() +
                                      " has no bin/SupComDataPath.lua");
        }
    }
    return search;
}

} // namespace osc::platform
