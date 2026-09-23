#include "platform/paths.hpp"

#include <cstdlib>
#include <fstream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#else
#include <pwd.h>
#include <unistd.h>
#endif

namespace osc::platform {

namespace fs = std::filesystem;

bool set_env_default(const char* name, const std::string& value) {
    if (std::getenv(name)) return false;
#ifdef _WIN32
    return _putenv_s(name, value.c_str()) == 0;
#else
    return setenv(name, value.c_str(), /*overwrite=*/0) == 0;
#endif
}

EnvLookup system_env() {
    return [](const char* key) -> std::optional<std::string> {
        const char* value = std::getenv(key);
        if (!value) return std::nullopt;
        return std::string(value);
    };
}

#ifdef _WIN32

namespace {

fs::path known_folder_path(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    fs::path result;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &raw))) {
        result = fs::path(raw); // UTF-16 -> path, no lossy narrowing
    }
    CoTaskMemFree(raw);
    return result;
}

} // namespace

fs::path home_dir(const EnvLookup& env) {
    if (auto profile = env("USERPROFILE"); profile && !profile->empty()) {
        return fs::path(*profile);
    }
    return known_folder_path(FOLDERID_Profile);
}

fs::path known_folder(KnownFolder folder, const EnvLookup& env) {
    switch (folder) {
    case KnownFolder::Documents:
        return known_folder_path(FOLDERID_Documents);
    case KnownFolder::LocalAppData:
    case KnownFolder::Config:
    case KnownFolder::Cache:
    case KnownFolder::State:
        break;
    }
    if (auto local = env("LOCALAPPDATA"); local && !local->empty()) {
        return fs::path(*local);
    }
    return known_folder_path(FOLDERID_LocalAppData);
}

#else

namespace {

/// An XDG variable is valid only if set, non-empty and absolute.
std::optional<fs::path> xdg_var(const EnvLookup& env, const char* key) {
    auto value = env(key);
    if (!value || value->empty()) return std::nullopt;
    fs::path p(*value);
    if (!p.is_absolute()) return std::nullopt;
    return p;
}

/// XDG_DOCUMENTS_DIR normally lives in $XDG_CONFIG_HOME/user-dirs.dirs as
/// `XDG_DOCUMENTS_DIR="$HOME/Documents"`, not in the environment.
std::optional<fs::path> user_dirs_documents(const fs::path& config_dir,
                                            const fs::path& home) {
    std::ifstream in(config_dir / "user-dirs.dirs");
    std::string line;
    const std::string key = "XDG_DOCUMENTS_DIR=";
    while (std::getline(in, line)) {
        if (line.rfind(key, 0) != 0) continue;
        std::string value = line.substr(key.size());
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
        }
        const std::string home_prefix = "$HOME";
        if (value.rfind(home_prefix, 0) == 0) {
            value = home.string() + value.substr(home_prefix.size());
        }
        fs::path p(value);
        if (p.is_absolute()) return p;
    }
    return std::nullopt;
}

} // namespace

fs::path home_dir(const EnvLookup& env) {
    if (auto home = xdg_var(env, "HOME")) return *home;
    if (const passwd* pw = getpwuid(getuid()); pw && pw->pw_dir) {
        return fs::path(pw->pw_dir);
    }
    return fs::path("/tmp");
}

fs::path known_folder(KnownFolder folder, const EnvLookup& env) {
    fs::path home = home_dir(env); // not const: returned by move below
    switch (folder) {
    case KnownFolder::Documents: {
        if (auto p = xdg_var(env, "XDG_DOCUMENTS_DIR")) return *p;
        const fs::path config = known_folder(KnownFolder::Config, env);
        if (auto p = user_dirs_documents(config, home)) return *p;
        return home / "Documents";
    }
    case KnownFolder::LocalAppData:
        return xdg_var(env, "XDG_DATA_HOME").value_or(home / ".local" / "share");
    case KnownFolder::Config:
        return xdg_var(env, "XDG_CONFIG_HOME").value_or(home / ".config");
    case KnownFolder::Cache:
        return xdg_var(env, "XDG_CACHE_HOME").value_or(home / ".cache");
    case KnownFolder::State:
        return xdg_var(env, "XDG_STATE_HOME").value_or(home / ".local" / "state");
    }
    return home;
}

#endif

fs::path known_folder(KnownFolder folder) {
    return known_folder(folder, system_env());
}

} // namespace osc::platform
