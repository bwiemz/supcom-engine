#include "lua/special_files.hpp"

#include "lua/lua_state.hpp"
#include "platform/paths.hpp"
#include "sim/replay.hpp"
#include "sim/sim_state.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iterator>
#include <system_error>

namespace osc::lua {

namespace fs = std::filesystem;

namespace {

constexpr const char* kRegistryKey = "__osc_special_files";

constexpr SpecialFiles::Type kTypes[] = {
    {"Replay", "replays", "oscreplay"},
    {"SaveGame", "savegames", "oscsave"},
};

const SpecialFiles::Type* check_type(lua_State* L, int idx) {
    const auto* type = SpecialFiles::find_type(luaL_checkstring(L, idx));
    if (!type) luaL_error(L, "unknown special file type '%s'", lua_tostring(L, idx));
    return type;
}

/// The file a (profile, base name, type) triple names, or empty when the
/// names aren't plain or there is no SpecialFiles.
fs::path file_of(lua_State* L, int first) {
    auto* files = get_special_files(L);
    const char* profile = luaL_checkstring(L, first);
    const char* base = luaL_checkstring(L, first + 1);
    const auto* type = check_type(L, first + 2);
    return files ? files->path(*type, profile, base) : fs::path();
}

void set_field(lua_State* L, const char* key, double value) {
    lua_pushstring(L, key);
    lua_pushnumber(L, value);
    lua_rawset(L, -3);
}

/// GetSpecialFiles(type) -> {directory = ".../replays/", extension =
/// "oscreplay", files = {[profile] = {base names}}}
int l_GetSpecialFiles(lua_State* L) {
    const auto* type = check_type(L, 1);
    auto* files = get_special_files(L);
    lua_newtable(L);
    lua_pushstring(L, "directory");
    std::string dir = files ? files->directory(*type).generic_string() : std::string();
    if (!dir.empty() && dir.back() != '/') dir += '/';
    lua_pushstring(L, dir.c_str());
    lua_rawset(L, -3);
    lua_pushstring(L, "extension");
    lua_pushstring(L, type->extension);
    lua_rawset(L, -3);
    lua_pushstring(L, "files");
    lua_newtable(L);
    if (files) {
        for (const auto& [profile, names] : files->list(*type)) {
            lua_pushstring(L, profile.c_str());
            lua_newtable(L);
            int i = 1;
            for (const auto& name : names) {
                lua_pushstring(L, name.c_str());
                lua_rawseti(L, -2, i++);
            }
            lua_rawset(L, -3);
        }
    }
    lua_rawset(L, -3);
    return 1;
}

/// GetSpecialFilePath(profile, base, type) -> the file's path
int l_GetSpecialFilePath(lua_State* L) {
    const fs::path path = file_of(L, 1);
    lua_pushstring(L, path.generic_string().c_str());
    return 1;
}

/// GetSpecialFileInfo(profile, base, type) -> {TimeStamp, WriteTime = {year,
/// month, mday, hour, minute, second}}, or nil when there is no such file.
int l_GetSpecialFileInfo(lua_State* L) {
    const fs::path path = file_of(L, 1);
    std::error_code ec;
    const auto when = path.empty() ? fs::file_time_type{} : fs::last_write_time(path, ec);
    if (path.empty() || ec) {
        lua_pushnil(L);
        return 1;
    }
    const auto sys = std::chrono::clock_cast<std::chrono::system_clock>(when);
    const std::time_t t = std::chrono::system_clock::to_time_t(sys);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    lua_newtable(L);
    set_field(L, "TimeStamp", static_cast<double>(t));
    lua_pushstring(L, "WriteTime");
    lua_newtable(L);
    set_field(L, "year", local.tm_year + 1900);
    set_field(L, "month", local.tm_mon + 1);
    set_field(L, "mday", local.tm_mday);
    set_field(L, "hour", local.tm_hour);
    set_field(L, "minute", local.tm_min);
    set_field(L, "second", local.tm_sec);
    lua_rawset(L, -3);
    return 1;
}

/// RemoveSpecialFile(profile, base, type)
int l_RemoveSpecialFile(lua_State* L) {
    const fs::path path = file_of(L, 1);
    std::error_code ec;
    if (!path.empty()) fs::remove(path, ec);
    return 0;
}

/// CopyCurrentReplay(profile, base) -- save the game being played, as
/// recorded so far, under that name.
int l_CopyCurrentReplay(lua_State* L) {
    const char* profile = luaL_checkstring(L, 1);
    const char* base = luaL_checkstring(L, 2);
    auto* files = get_special_files(L);
    lua_pushstring(L, "osc_sim_state");
    lua_rawget(L, LUA_REGISTRYINDEX);
    const auto* sim = static_cast<const sim::SimState*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    if (!files || !sim || !sim->recording()) return 0;
    const auto path = files->path(*SpecialFiles::find_type("Replay"), profile, base);
    if (!path.empty()) write_replay_file(sim->recorded_replay(), path);
    return 0;
}

/// LaunchReplaySession(file) -> true if the file is a replay that can start
/// its game; the game loop then launches it (see the launch request in
/// main.cpp).
int l_LaunchReplaySession(lua_State* L) {
    const char* file = luaL_checkstring(L, 1);
    auto replay = read_replay_file(file);
    if (!replay) {
        lua_pushboolean(L, 0);
        return 1;
    }
    lua_pushstring(L, "__osc_launch_replay");
    lua_pushstring(L, file);
    lua_rawset(L, LUA_REGISTRYINDEX);
    lua_pushstring(L, "__osc_launch_scenario");
    lua_pushstring(L, replay->setup.scenario.c_str());
    lua_rawset(L, LUA_REGISTRYINDEX);
    lua_pushstring(L, "__osc_launch_requested");
    lua_pushboolean(L, 1);
    lua_rawset(L, LUA_REGISTRYINDEX);
    lua_pushboolean(L, 1);
    return 1;
}

} // namespace

fs::path SpecialFiles::default_root() {
    return platform::known_folder(platform::KnownFolder::Documents) / "My Games" /
           "Gas Powered Games" / "Supreme Commander Forged Alliance";
}

const SpecialFiles::Type* SpecialFiles::find_type(std::string_view name) {
    for (const auto& type : kTypes)
        if (name == type.name) return &type;
    return nullptr;
}

bool SpecialFiles::plain_name(std::string_view name) {
    return !name.empty() && name != "." && name != ".." &&
           name.find_first_of("/\\:") == std::string_view::npos;
}

fs::path SpecialFiles::path(const Type& type, std::string_view profile,
                            std::string_view base) const {
    if (!plain_name(profile) || !plain_name(base)) return {};
    return directory(type) / std::string(profile) / (std::string(base) + "." + type.extension);
}

std::map<std::string, std::vector<std::string>> SpecialFiles::list(const Type& type) const {
    std::map<std::string, std::vector<std::string>> out;
    std::error_code ec;
    const std::string ext = std::string(".") + type.extension;
    for (const auto& profile : fs::directory_iterator(directory(type), ec)) {
        if (!profile.is_directory()) continue;
        std::vector<std::string> names;
        for (const auto& file : fs::directory_iterator(profile.path(), ec)) {
            if (file.is_regular_file() && file.path().extension() == ext)
                names.push_back(file.path().stem().string());
        }
        if (names.empty()) continue; // e.g. a folder of FA's own replays
        std::sort(names.begin(), names.end());
        out[profile.path().filename().string()] = std::move(names);
    }
    return out;
}

SpecialFiles* get_special_files(lua_State* L) {
    lua_pushstring(L, kRegistryKey);
    lua_rawget(L, LUA_REGISTRYINDEX);
    auto* files = static_cast<SpecialFiles*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return files;
}

bool write_replay_file(const sim::Replay& replay, const fs::path& path) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    const auto bytes = replay.serialize();
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    if (!out) {
        spdlog::error("Replay: cannot write {}", path.string());
        return false;
    }
    spdlog::info("Replay: {} commands over {} ticks written to {}", replay.commands.size(),
                 replay.final_tick, path.string());
    return true;
}

std::optional<sim::Replay> read_replay_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        spdlog::error("Replay: cannot read {}", path.string());
        return std::nullopt;
    }
    const std::vector<u8> bytes((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
    sim::Replay replay;
    if (!sim::Replay::deserialize(bytes, replay)) {
        spdlog::error("Replay: {} is not a replay, or is damaged", path.string());
        return std::nullopt;
    }
    if (!replay.has_setup) {
        spdlog::error("Replay: {} (format {}) has no game setup to start from", path.string(),
                      replay.version);
        return std::nullopt;
    }
    return replay;
}

void register_special_file_bindings(LuaState& state, SpecialFiles* files) {
    lua_State* L = state.raw();
    lua_pushstring(L, kRegistryKey);
    lua_pushlightuserdata(L, files);
    lua_rawset(L, LUA_REGISTRYINDEX);
    state.register_function("GetSpecialFiles", l_GetSpecialFiles);
    state.register_function("GetSpecialFilePath", l_GetSpecialFilePath);
    state.register_function("GetSpecialFileInfo", l_GetSpecialFileInfo);
    state.register_function("RemoveSpecialFile", l_RemoveSpecialFile);
    state.register_function("CopyCurrentReplay", l_CopyCurrentReplay);
    state.register_function("LaunchReplaySession", l_LaunchReplaySession);
}

} // namespace osc::lua
