#include "lua/command_line_mods.hpp"

#include "lua/init_loader.hpp"
#include "lua/lua_state.hpp"
#include "sim/lua_bytes.hpp"
#include "vfs/virtual_file_system.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fmt/format.h>
#include <spdlog/spdlog.h>
#include <system_error>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace osc::lua {

namespace {

/// The resolution, a function of the --mod list ({given, at}: `at` the
/// mount point of a mod on disk) run with the data's mods.lua. It returns
/// GetGameMods of the selection, the UI-only mods' uids, the required uids
/// that joined, and each picked mod's "name (uid)"; it raises what refuses
/// the list. What a mod requires and conflicts with is the mod manager's
/// (retail's modmanager.lua asks GetDependencies as a mod is picked):
/// there the player is asked; here the requirements join, and a missing
/// one or a conflict fails.
constexpr const char* kResolve = R"(
return function(args)
    local Mods = import('/lua/mods.lua')
    local all = Mods.AllMods()

    local function sorted_keys(set)
        local keys = {}
        for key in pairs(set) do table.insert(keys, tostring(key)) end
        table.sort(keys)
        return keys
    end
    local function named(uid)
        local info = all[uid]
        return tostring(info and info.name or uid) .. ' (' .. tostring(uid) .. ')'
    end
    local function installed()
        local lines = {}
        for _, uid in ipairs(sorted_keys(all)) do table.insert(lines, '\n  ' .. named(uid)) end
        if table.getn(lines) == 0 then return ' none' end
        return table.concat(lines)
    end

    local selected = {}
    for _, mod in ipairs(args) do
        if mod.at then
            -- A mod on disk: the mods under its mount point
            local found = false
            local below = mod.at .. '/'
            for uid, info in pairs(all) do
                local at = string.lower(tostring(info.location))
                if at == mod.at or string.sub(at, 1, string.len(below)) == below then
                    selected[uid] = true
                    found = true
                end
            end
            if not found then
                error('--mod ' .. mod.given .. ': no mod at ' .. mod.at .. ' (it has no '
                      .. 'mod_info.lua that loads, or one with enabled = false, or another '
                      .. 'installed mod has its uid)', 0)
            end
        else
            local uid = all[mod.given] and mod.given
            if not uid then
                local lower = string.lower(mod.given)
                for key in pairs(all) do
                    if string.lower(tostring(key)) == lower then uid = key end
                end
            end
            if not uid then
                error('--mod ' .. mod.given .. ': no installed mod has that uid. The installed '
                      .. 'mods:' .. installed(), 0)
            end
            selected[uid] = true
        end
    end

    local required = {}
    for _, uid in ipairs(sorted_keys(selected)) do
        local needs = Mods.GetDependencies(uid)
        if needs.missing then
            local names = all[uid].requiresNames or {}
            local missing = {}
            for _, need in ipairs(sorted_keys(needs.missing)) do
                local name = names[need]
                table.insert(missing, name and (tostring(name) .. ' (' .. need .. ')') or need)
            end
            error('--mod: ' .. named(uid) .. ' requires mods that are not installed: '
                  .. table.concat(missing, ', '), 0)
        end
        for need in pairs(needs.requires or {}) do
            if not selected[need] then required[need] = true end
        end
    end
    for need in pairs(required) do selected[need] = true end

    local picked = sorted_keys(selected)
    for _, uid in ipairs(picked) do
        for other in pairs(Mods.GetDependencies(uid).conflicts or {}) do
            if other ~= uid and selected[other] then
                error('--mod: ' .. named(uid) .. ' conflicts with ' .. named(other), 0)
            end
        end
        if all[uid].exclusive and table.getn(picked) > 1 then
            error('--mod: ' .. named(uid) .. ' is exclusive: it plays with no other mod', 0)
        end
    end

    local ui, labels = {}, {}
    for _, uid in ipairs(picked) do
        if all[uid].ui_only then table.insert(ui, uid) end
        labels[uid] = named(uid)
    end
    return Mods.GetGameMods(selected), ui, sorted_keys(required), labels
end
)";

/// The strings of the list at `idx` (absolute), in order, to its first nil.
std::vector<std::string> string_list(lua_State* L, int idx) {
    std::vector<std::string> out;
    for (int i = 1;; ++i) {
        lua_rawgeti(L, idx, i);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            return out;
        }
        if (lua_type(L, -1) == LUA_TSTRING) out.emplace_back(lua_tostring(L, -1));
        lua_pop(L, 1);
    }
}

/// labels[uid] at `labels` (absolute) for each of `uids`, joined.
std::string labelled(lua_State* L, int labels, const std::vector<std::string>& uids) {
    std::string out;
    for (const auto& uid : uids) {
        lua_pushstring(L, uid.c_str());
        lua_rawget(L, labels);
        if (!out.empty()) out += ", ";
        out += lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : uid;
        lua_pop(L, 1);
    }
    return out.empty() ? "none" : out;
}

bool is_archive(const fs::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".scd" || ext == ".zip";
}

} // namespace

ModArg mod_arg(std::string_view given) {
    ModArg arg{std::string(given), {}, {}};
    std::error_code ec;
    const fs::path path(arg.given);
    if (arg.given.empty() || !fs::exists(path, ec)) return arg; // a uid
    // (Named as given, a link too, as retail's init names a mods folder entry)
    fs::path full = fs::absolute(path, ec);
    if (ec) full = path;
    arg.path = full.lexically_normal();
    arg.mount_point = mod_mount_point(arg.path);
    return arg;
}

std::string mod_mount_point(const fs::path& path) {
    fs::path file = path.filename();
    if (file.empty()) file = path.parent_path().filename(); // "folder/"
    std::string name = file.string();
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    // string.gsub(mp, '[.]scd$', ''), then '[.]zip$'
    for (const std::string_view ext : {".scd", ".zip"}) {
        if (name.size() >= ext.size() && name.ends_with(ext)) name.resize(name.size() - ext.size());
    }
    return "/mods/" + name;
}

std::string check_mod_args(const std::vector<ModArg>& mods) {
    for (size_t i = 0; i < mods.size(); ++i) {
        const ModArg& mod = mods[i];
        if (mod.path.empty()) continue;
        std::error_code ec;
        const bool folder = fs::is_directory(mod.path, ec);
        const bool archive = !folder && fs::is_regular_file(mod.path, ec) && is_archive(mod.path);
        if (!folder && !archive) {
            return fmt::format("--mod {}: a mod on disk is a folder, or a .zip or .scd archive",
                               mod.given);
        }
        for (size_t j = 0; j < i; ++j) {
            if (!mods[j].path.empty() && mods[j].mount_point == mod.mount_point) {
                return fmt::format("--mod {} and --mod {}: both would mount at {}", mods[j].given,
                                   mod.given, mod.mount_point);
            }
        }
    }
    return {};
}

Result<CommandLineMods> resolve_command_line_mods(vfs::VirtualFileSystem& vfs,
                                                  const std::vector<ModArg>& mods) {
    if (!vfs.file_exists("/lua/mods.lua"))
        return Error("--mod: the game's data has no /lua/mods.lua to find its mods with");
    LuaState state;
    if (auto ready = prepare_script_state(state, vfs); !ready)
        return Error("--mod: " + ready.error().message);
    lua_State* L = state.raw();
    if (luaL_loadbuffer(L, kResolve, std::strlen(kResolve), "=--mod") != 0 ||
        lua_pcall(L, 0, 1, 0) != 0) {
        return Error(std::string("--mod: ") +
                     (lua_isstring(L, -1) ? lua_tostring(L, -1) : "the resolution won't load"));
    }
    lua_newtable(L);
    int n = 0;
    for (const ModArg& mod : mods) {
        lua_newtable(L);
        lua_pushstring(L, "given");
        lua_pushstring(L, mod.given.c_str());
        lua_rawset(L, -3);
        if (!mod.path.empty()) {
            lua_pushstring(L, "at");
            lua_pushstring(L, vfs::VirtualFileSystem::normalize(mod.mount_point).c_str());
            lua_rawset(L, -3);
        }
        lua_rawseti(L, -2, ++n);
    }
    if (lua_pcall(L, 1, 4, 0) != 0) {
        return Error(lua_isstring(L, -1) ? lua_tostring(L, -1)
                                         : "--mod: the mods couldn't be resolved");
    }
    const int game = lua_gettop(L) - 3;
    const int labels = game + 3;
    CommandLineMods out;
    auto bytes = sim::lua_to_bytes(L, game);
    if (!lua_istable(L, game) || !bytes) {
        return Error("--mod: mods.lua's GetGameMods gave no list a game can record (nested too "
                     "deep, or too large)");
    }
    out.game = std::move(*bytes);
    for (int i = 1;; ++i) {
        lua_rawgeti(L, game, i);
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            break;
        }
        lua_pushstring(L, "uid");
        lua_rawget(L, -2);
        if (lua_type(L, -1) == LUA_TSTRING) out.game_uids.emplace_back(lua_tostring(L, -1));
        lua_pop(L, 2);
    }
    out.ui_uids = string_list(L, game + 1);
    out.required = string_list(L, game + 2);

    spdlog::info("Game mods (--mod): {}", labelled(L, labels, out.game_uids));
    spdlog::info("UI mods (--mod): {}", labelled(L, labels, out.ui_uids));
    if (!out.required.empty())
        spdlog::info("Required by them: {}", labelled(L, labels, out.required));
    return out;
}

} // namespace osc::lua
