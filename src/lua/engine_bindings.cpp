#include "lua/engine_bindings.hpp"
#include "lua/lua_state.hpp"
#include "core/log.hpp"
#include "vfs/virtual_file_system.hpp"

#include <algorithm>
#include <filesystem>
#include <random>
#include <spdlog/spdlog.h>

#ifdef _WIN32
#include <ShlObj.h>
#pragma comment(lib, "shell32.lib")
#endif

extern "C" {
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
}

namespace osc::lua {

namespace fs = std::filesystem;

// ============================================================================
// Init context bindings (real filesystem, no VFS)
// ============================================================================

/// io.dir(pattern) — returns a table of filenames matching a directory glob.
/// The pattern is like "C:/path/to/dir/*" or "C:/path/to/dir/*.scd"
static int l_io_dir(lua_State* L) {
    const char* pattern = luaL_checkstring(L, 1);
    std::string pat(pattern);

    // Split into directory and file pattern
    auto last_sep = pat.find_last_of("/\\");
    std::string dir_str, file_pat;
    if (last_sep != std::string::npos) {
        dir_str = pat.substr(0, last_sep);
        file_pat = pat.substr(last_sep + 1);
    } else {
        dir_str = ".";
        file_pat = pat;
    }

    // Determine suffix to match
    std::string suffix;
    if (!file_pat.empty() && file_pat[0] == '*') {
        suffix = file_pat.substr(1);
    }
    bool match_all = (file_pat == "*" || file_pat == "*.*");

    lua_newtable(L);
    int idx = 1;

    std::error_code ec;
    fs::path dir_path(dir_str);
    if (fs::exists(dir_path, ec) && fs::is_directory(dir_path, ec)) {
        for (auto& entry : fs::directory_iterator(dir_path, ec)) {
            std::string name = entry.path().filename().string();

            bool match = match_all;
            if (!match && !suffix.empty()) {
                std::string name_lower = name;
                std::transform(name_lower.begin(), name_lower.end(),
                               name_lower.begin(),
                               [](unsigned char c) { return std::tolower(c); });
                std::string suffix_lower = suffix;
                std::transform(suffix_lower.begin(), suffix_lower.end(),
                               suffix_lower.begin(),
                               [](unsigned char c) { return std::tolower(c); });
                if (name_lower.size() >= suffix_lower.size()) {
                    match = name_lower.compare(
                        name_lower.size() - suffix_lower.size(),
                        suffix_lower.size(), suffix_lower) == 0;
                }
            }

            if (match) {
                lua_pushnumber(L, idx++);
                lua_pushstring(L, name.c_str());
                lua_settable(L, -3);
            }
        }
    }

    return 1;
}

/// SHGetFolderPath(name) — returns Windows special folder paths.
static int l_SHGetFolderPath(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    std::string result;

#ifdef _WIN32
    wchar_t path[MAX_PATH];
    if (std::strcmp(name, "LOCAL_APPDATA") == 0) {
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr,
                                        0, path))) {
            int len = WideCharToMultiByte(CP_UTF8, 0, path, -1, nullptr, 0, nullptr, nullptr);
            result.resize(len - 1);
            WideCharToMultiByte(CP_UTF8, 0, path, -1, result.data(), len, nullptr, nullptr);
            result += "/";
        }
    } else if (std::strcmp(name, "PERSONAL") == 0) {
        if (SUCCEEDED(
                SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, 0, path))) {
            int len = WideCharToMultiByte(CP_UTF8, 0, path, -1, nullptr, 0, nullptr, nullptr);
            result.resize(len - 1);
            WideCharToMultiByte(CP_UTF8, 0, path, -1, result.data(), len, nullptr, nullptr);
            result += "/";
        }
    }
#else
    const char* home = std::getenv("HOME");
    if (!home) home = "/tmp";
    if (std::strcmp(name, "LOCAL_APPDATA") == 0) {
        result = std::string(home) + "/.local/share/";
    } else if (std::strcmp(name, "PERSONAL") == 0) {
        result = std::string(home) + "/Documents/";
    }
#endif

    // Normalize to forward slashes
    std::replace(result.begin(), result.end(), '\\', '/');
    lua_pushstring(L, result.c_str());
    return 1;
}

/// Stub: SetProcessPriority — returns true.
static int l_SetProcessPriority(lua_State* L) {
    lua_pushboolean(L, 1);
    return 1;
}

/// Stub: GetProcessAffinityMask — returns true, mask, mask.
static int l_GetProcessAffinityMask(lua_State* L) {
    lua_pushboolean(L, 1);
    lua_pushnumber(L, 255);
    lua_pushnumber(L, 255);
    return 3;
}

/// Stub: SetProcessAffinityMask — returns true.
static int l_SetProcessAffinityMask(lua_State* L) {
    lua_pushboolean(L, 1);
    return 1;
}

// ============================================================================
// Blueprint context bindings (VFS active)
// ============================================================================

/// doscript(path, env?) — load a file from VFS and execute it.
static int l_doscript(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);

    auto* vfs = LuaState::get_vfs(L);
    if (!vfs) {
        return luaL_error(L, "doscript: VFS not initialized");
    }

    auto data = vfs->read_file(path);
    if (!data) {
        return luaL_error(L, "doscript: file not found: %s", path);
    }

    // Strip UTF-8 BOM if present (e.g. loc/us/strings_db.lua)
    const char* buf = data->data();
    size_t len = data->size();
    if (len >= 3 && static_cast<unsigned char>(buf[0]) == 0xEF &&
        static_cast<unsigned char>(buf[1]) == 0xBB &&
        static_cast<unsigned char>(buf[2]) == 0xBF) {
        buf += 3;
        len -= 3;
    }

    // Load the chunk with the virtual path as chunk name
    std::string chunk_name = std::string("@") + path;
    int status = luaL_loadbuffer(L, buf, len, chunk_name.c_str());
    if (status != 0) {
        return lua_error(L);
    }

    // If env table provided as second argument, use it as the function env
    if (lua_istable(L, 2)) {
        lua_pushvalue(L, 2);
        lua_setfenv(L, -2);
    }

    // Execute
    status = lua_pcall(L, 0, 0, 0);
    if (status != 0) {
        return lua_error(L);
    }

    return 0;
}

/// DiskFindFiles(directory, pattern) — search VFS for matching files.
static int l_DiskFindFiles(lua_State* L) {
    const char* directory = luaL_checkstring(L, 1);
    const char* pattern = luaL_checkstring(L, 2);

    auto* vfs = LuaState::get_vfs(L);
    if (!vfs) {
        lua_newtable(L);
        return 1;
    }

    auto files = vfs->find_files(directory, pattern);

    lua_newtable(L);
    for (size_t i = 0; i < files.size(); i++) {
        lua_pushnumber(L, static_cast<double>(i + 1));
        lua_pushstring(L, files[i].c_str());
        lua_settable(L, -3);
    }

    return 1;
}

/// DiskGetFileInfo(filename) — returns file info table or false.
static int l_DiskGetFileInfo(lua_State* L) {
    const char* filename = luaL_checkstring(L, 1);

    auto* vfs = LuaState::get_vfs(L);
    if (!vfs) {
        lua_pushboolean(L, 0);
        return 1;
    }

    auto info = vfs->get_file_info(filename);
    if (!info) {
        lua_pushboolean(L, 0);
        return 1;
    }

    lua_newtable(L);
    lua_pushstring(L, "SizeBytes");
    lua_pushnumber(L, static_cast<double>(info->size_bytes));
    lua_settable(L, -3);
    lua_pushstring(L, "IsFolder");
    lua_pushboolean(L, info->is_folder ? 1 : 0);
    lua_settable(L, -3);

    return 1;
}

/// exists(name) — check if file exists in VFS.
static int l_exists(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    auto* vfs = LuaState::get_vfs(L);
    lua_pushboolean(L, (vfs && vfs->file_exists(name)) ? 1 : 0);
    return 1;
}

/// DiskToLocal(path) — identity for now.
static int l_DiskToLocal(lua_State* L) {
    lua_pushvalue(L, 1);
    return 1;
}

/// Basename(path, stripExtension?) — return last component of a path.
static int l_Basename(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    bool strip_ext = lua_toboolean(L, 2) != 0;

    std::string p(path);
    auto last_sep = p.find_last_of("/\\");
    std::string basename =
        (last_sep != std::string::npos) ? p.substr(last_sep + 1) : p;

    if (strip_ext) {
        auto dot = basename.find_last_of('.');
        if (dot != std::string::npos) {
            basename = basename.substr(0, dot);
        }
    }

    lua_pushstring(L, basename.c_str());
    return 1;
}

/// Dirname(path) — return directory part of a path.
static int l_Dirname(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    std::string p(path);
    auto last_sep = p.find_last_of("/\\");
    if (last_sep != std::string::npos) {
        lua_pushstring(L, p.substr(0, last_sep).c_str());
    } else {
        lua_pushstring(L, "");
    }
    return 1;
}

/// FileCollapsePath(path) — collapse .. and . in paths.
static int l_FileCollapsePath(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    auto normalized = vfs::VirtualFileSystem::normalize(path);
    lua_pushstring(L, normalized.c_str());
    return 1;
}

/// Sound({Bank, Cue, ...}) — identity function, returns the table as-is.
static int l_Sound(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_pushvalue(L, 1);
    return 1;
}

/// ParseEntityCategory(cat) — return the string as-is for now.
static int l_ParseEntityCategory(lua_State* L) {
    lua_pushvalue(L, 1);
    return 1;
}

/// EntityCategoryContains — stub returning false.
static int l_EntityCategoryContains(lua_State* L) {
    lua_pushboolean(L, 0);
    return 1;
}

/// EntityCategoryEmpty — stub returning true.
static int l_EntityCategoryEmpty(lua_State* L) {
    lua_pushboolean(L, 1);
    return 1;
}

/// EntityCategoryFilterDown — stub returning empty table.
static int l_EntityCategoryFilterDown(lua_State* L) {
    lua_newtable(L);
    return 1;
}

/// EntityCategoryGetUnitList — stub returning empty table.
static int l_EntityCategoryGetUnitList(lua_State* L) {
    lua_newtable(L);
    return 1;
}

/// GetVersion() — return a version string.
static int l_GetVersion(lua_State* L) {
    lua_pushstring(L, "1.0.0");
    return 1;
}

/// Random(min?, max?) — generate a random number.
static int l_Random(lua_State* L) {
    static std::mt19937 gen(std::random_device{}());
    int n = lua_gettop(L);
    if (n == 0) {
        std::uniform_real_distribution<double> dist(0.0, 1.0);
        lua_pushnumber(L, dist(gen));
    } else if (n == 1) {
        int max = static_cast<int>(luaL_checknumber(L, 1));
        std::uniform_int_distribution<int> dist(1, max);
        lua_pushnumber(L, dist(gen));
    } else {
        int min = static_cast<int>(luaL_checknumber(L, 1));
        int max = static_cast<int>(luaL_checknumber(L, 2));
        std::uniform_int_distribution<int> dist(min, max);
        lua_pushnumber(L, dist(gen));
    }
    return 1;
}

/// BlueprintLoaderUpdateProgress() — no-op.
static int l_BlueprintLoaderUpdateProgress(lua_State*) {
    return 0;
}

/// GetGameTimeSeconds() — return 0.
static int l_GetGameTimeSeconds(lua_State* L) {
    lua_pushnumber(L, 0);
    return 1;
}

/// GetFocusArmy() — return -1 (observer).
static int l_GetFocusArmy(lua_State* L) {
    lua_pushnumber(L, -1);
    return 1;
}

/// SessionIsReplay() — return false.
static int l_SessionIsReplay(lua_State* L) {
    lua_pushboolean(L, 0);
    return 1;
}

/// MATH_IRound(n) — round to nearest integer (banker's rounding).
static int l_MATH_IRound(lua_State* L) {
    double n = luaL_checknumber(L, 1);
    lua_pushnumber(L, std::nearbyint(n));
    return 1;
}

/// Rect(x0, y0, x1, y1) — return a table.
static int l_Rect(lua_State* L) {
    lua_newtable(L);
    lua_pushstring(L, "x0"); lua_pushvalue(L, 1); lua_settable(L, -3);
    lua_pushstring(L, "y0"); lua_pushvalue(L, 2); lua_settable(L, -3);
    lua_pushstring(L, "x1"); lua_pushvalue(L, 3); lua_settable(L, -3);
    lua_pushstring(L, "y1"); lua_pushvalue(L, 4); lua_settable(L, -3);
    return 1;
}

// Registry key for the shared vector metatable
static const char* REG_VECTOR_MT = "__osc_vector_mt";

static void push_vector_metatable(lua_State* L) {
    lua_pushstring(L, REG_VECTOR_MT);
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (lua_isnil(L, -1)) {
        // First time: create the metatable
        lua_pop(L, 1);
        lua_newtable(L);
        // Store in registry
        lua_pushstring(L, REG_VECTOR_MT);
        lua_pushvalue(L, -2);
        lua_rawset(L, LUA_REGISTRYINDEX);
    }
}

/// Vector(x, y, z) — return a table with shared metatable.
static int l_Vector(lua_State* L) {
    double x = luaL_checknumber(L, 1);
    double y = luaL_checknumber(L, 2);
    double z = luaL_checknumber(L, 3);
    lua_newtable(L);
    lua_pushnumber(L, 1); lua_pushnumber(L, x); lua_settable(L, -3);
    lua_pushnumber(L, 2); lua_pushnumber(L, y); lua_settable(L, -3);
    lua_pushnumber(L, 3); lua_pushnumber(L, z); lua_settable(L, -3);
    push_vector_metatable(L);
    lua_setmetatable(L, -2);
    return 1;
}

/// Vector2(x, y) — return a 2-component table with shared metatable.
static int l_Vector2(lua_State* L) {
    double x = luaL_checknumber(L, 1);
    double y = luaL_checknumber(L, 2);
    lua_newtable(L);
    lua_pushnumber(L, 1); lua_pushnumber(L, x); lua_settable(L, -3);
    lua_pushnumber(L, 2); lua_pushnumber(L, y); lua_settable(L, -3);
    push_vector_metatable(L);
    lua_setmetatable(L, -2);
    return 1;
}

/// Vector3(x, y, z) — alias for Vector.
static int l_Vector3(lua_State* L) {
    return l_Vector(L);
}

// Thread stubs
static int l_ForkThread(lua_State* L) { lua_pushnil(L); return 1; }
static int l_KillThread(lua_State*) { return 0; }
static int l_CurrentThread(lua_State* L) { lua_pushnil(L); return 1; }
static int l_SuspendCurrentThread(lua_State*) { return 0; }
static int l_ResumeThread(lua_State*) { return 0; }
static int l_WaitFor(lua_State*) { return 0; }

// Misc stubs
static int l_Trace(lua_State*) { return 0; }
static int l_BeginLoggingStats(lua_State*) { return 0; }
static int l_EndLoggingStats(lua_State*) { return 0; }
static int l_AITarget(lua_State*) { return 0; }
static int l_SecondsPerTick(lua_State* L) { lua_pushnumber(L, 0.1); return 1; }

/// STR_GetTokens(string, delimiter)
static int l_STR_GetTokens(lua_State* L) {
    const char* str = luaL_checkstring(L, 1);
    const char* delim = luaL_checkstring(L, 2);

    lua_newtable(L);
    int idx = 1;
    std::string s(str);
    std::string d(delim);

    size_t pos = 0;
    while (true) {
        auto found = s.find(d, pos);
        std::string token = s.substr(pos, found - pos);
        if (!token.empty()) {
            lua_pushnumber(L, idx++);
            lua_pushstring(L, token.c_str());
            lua_settable(L, -3);
        }
        if (found == std::string::npos) break;
        pos = found + d.size();
    }

    return 1;
}

/// STR_Utf8Len
static int l_STR_Utf8Len(lua_State* L) {
    const char* str = luaL_checkstring(L, 1);
    size_t len = 0;
    while (*str) {
        if ((*str & 0xC0) != 0x80) len++;
        str++;
    }
    lua_pushnumber(L, static_cast<double>(len));
    return 1;
}

/// STR_Utf8SubString
static int l_STR_Utf8SubString(lua_State* L) {
    const char* str = luaL_checkstring(L, 1);
    int start = static_cast<int>(luaL_checknumber(L, 2));
    int count = static_cast<int>(luaL_checknumber(L, 3));

    std::string s(str);
    // Simple byte-based substring for now
    if (start < 0) start = 0;
    if (start >= static_cast<int>(s.size())) {
        lua_pushstring(L, "");
        return 1;
    }
    lua_pushstring(L, s.substr(start, count).c_str());
    return 1;
}

/// STR_itox / STR_xtoi
static int l_STR_itox(lua_State* L) {
    int n = static_cast<int>(luaL_checknumber(L, 1));
    char buf[32];
    snprintf(buf, sizeof(buf), "%x", n);
    lua_pushstring(L, buf);
    return 1;
}

static int l_STR_xtoi(lua_State* L) {
    const char* str = luaL_checkstring(L, 1);
    unsigned int val = 0;
    sscanf(str, "%x", &val);
    lua_pushnumber(L, val);
    return 1;
}

/// EnumColorNames — return empty table.
static int l_EnumColorNames(lua_State* L) {
    lua_newtable(L);
    return 1;
}

/// IsDestroyed — return false.
static int l_IsDestroyed(lua_State* L) {
    lua_pushboolean(L, 0);
    return 1;
}

// ============================================================================
// Registration
// ============================================================================

void register_init_bindings(LuaState& state) {
    // Logging
    state.register_function("LOG", log::l_LOG);
    state.register_function("WARN", log::l_WARN);
    state.register_function("SPEW", log::l_SPEW);
    state.register_function("_ALERT", log::l_ALERT);

    // io.dir
    state.register_table_function("io", "dir", l_io_dir);

    // Windows special folders
    state.register_function("SHGetFolderPath", l_SHGetFolderPath);

    // Process management stubs (checked via rawget in init_faf.lua)
    state.register_function("SetProcessPriority", l_SetProcessPriority);
    state.register_function("GetProcessAffinityMask", l_GetProcessAffinityMask);
    state.register_function("SetProcessAffinityMask", l_SetProcessAffinityMask);
}

void register_blueprint_bindings(LuaState& state) {
    // VFS-backed file operations
    state.register_function("doscript", l_doscript);
    state.register_function("DiskFindFiles", l_DiskFindFiles);
    state.register_function("DiskGetFileInfo", l_DiskGetFileInfo);
    state.register_function("exists", l_exists);
    state.register_function("DiskToLocal", l_DiskToLocal);

    // Path utilities
    state.register_function("Basename", l_Basename);
    state.register_function("Dirname", l_Dirname);
    state.register_function("FileCollapsePath", l_FileCollapsePath);

    // Blueprint registration helpers
    state.register_function("Sound", l_Sound);
    state.register_function("ParseEntityCategory", l_ParseEntityCategory);
    state.register_function("EntityCategoryContains", l_EntityCategoryContains);
    state.register_function("EntityCategoryEmpty", l_EntityCategoryEmpty);
    state.register_function("EntityCategoryFilterDown", l_EntityCategoryFilterDown);
    state.register_function("EntityCategoryGetUnitList", l_EntityCategoryGetUnitList);
    state.register_function("BlueprintLoaderUpdateProgress", l_BlueprintLoaderUpdateProgress);
    state.register_function("GetVersion", l_GetVersion);
    state.register_function("Random", l_Random);

    // Math / Vector
    state.register_function("MATH_IRound", l_MATH_IRound);
    state.register_function("Rect", l_Rect);
    state.register_function("Vector", l_Vector);
    state.register_function("Vector2", l_Vector2);
    state.register_function("Vector3", l_Vector3);

    // Game state stubs
    state.register_function("GetGameTimeSeconds", l_GetGameTimeSeconds);
    state.register_function("GetFocusArmy", l_GetFocusArmy);
    state.register_function("SessionIsReplay", l_SessionIsReplay);
    state.register_function("SecondsPerTick", l_SecondsPerTick);
    state.register_function("IsDestroyed", l_IsDestroyed);

    // Thread stubs
    state.register_function("ForkThread", l_ForkThread);
    state.register_function("KillThread", l_KillThread);
    state.register_function("CurrentThread", l_CurrentThread);
    state.register_function("SuspendCurrentThread", l_SuspendCurrentThread);
    state.register_function("ResumeThread", l_ResumeThread);
    state.register_function("WaitFor", l_WaitFor);

    // String utilities
    state.register_function("STR_GetTokens", l_STR_GetTokens);
    state.register_function("STR_Utf8Len", l_STR_Utf8Len);
    state.register_function("STR_Utf8SubString", l_STR_Utf8SubString);
    state.register_function("STR_itox", l_STR_itox);
    state.register_function("STR_xtoi", l_STR_xtoi);
    state.register_function("EnumColorNames", l_EnumColorNames);

    // Misc stubs
    state.register_function("Trace", l_Trace);
    state.register_function("BeginLoggingStats", l_BeginLoggingStats);
    state.register_function("EndLoggingStats", l_EndLoggingStats);
    state.register_function("AITarget", l_AITarget);

    // Pre-set globals expected by the Lua code
    state.set_global_table("__diskwatch");
    state.set_global_table("__modules");
    state.set_global_table("__active_mods");
}

} // namespace osc::lua
