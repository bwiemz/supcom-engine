#include "core/localization.hpp"
#include "vfs/virtual_file_system.hpp"
#include <spdlog/spdlog.h>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

namespace osc::core {

void Localization::add(const std::string& key, const std::string& value) {
    strings_[key] = value;
}

void Localization::load_from_vfs(lua_State* L, vfs::VirtualFileSystem* vfs) {
    auto opt = vfs->read_file("/loc/us/strings_db.lua");
    if (!opt.has_value() || opt->empty()) {
        spdlog::warn("Localization: /loc/us/strings_db.lua not found or empty");
        return;
    }
    const auto& data = *opt;

    lua_State* tmp = lua_open();
    luaopen_base(tmp);
    luaopen_string(tmp);

    if (luaL_loadbuffer(tmp, data.data(), data.size(), "strings_db") != 0 ||
        lua_pcall(tmp, 0, 0, 0) != 0) {
        spdlog::warn("Localization: failed to execute strings_db.lua: {}",
                     lua_tostring(tmp, -1));
        lua_close(tmp);
        return;
    }

    lua_pushvalue(tmp, LUA_GLOBALSINDEX);
    lua_pushnil(tmp);
    while (lua_next(tmp, -2) != 0) {
        if (lua_type(tmp, -2) == LUA_TSTRING &&
            lua_type(tmp, -1) == LUA_TSTRING) {
            const char* k = lua_tostring(tmp, -2);
            const char* v = lua_tostring(tmp, -1);
            if (k && v) {
                strings_[k] = v;
                strings_[std::string("<LOC ") + k + ">"] = v;
            }
        }
        lua_pop(tmp, 1);
    }
    lua_pop(tmp, 1);

    lua_close(tmp);
    spdlog::info("Localization: loaded {} strings", strings_.size());
}

const std::string& Localization::lookup(const std::string& key) const {
    auto it = strings_.find(key);
    if (it != strings_.end()) return it->second;

    // Try stripping <LOC ...> wrapper
    if (key.size() > 6 && key.substr(0, 5) == "<LOC " && key.back() == '>') {
        auto inner = key.substr(5, key.size() - 6);
        auto it2 = strings_.find(inner);
        if (it2 != strings_.end()) return it2->second;
    }

    // FA convention: return key itself if not found
    fallback_ = key;
    return fallback_;
}

std::string Localization::format(const std::string& key,
                                  const std::vector<std::string>& args) const {
    const auto& fmt = lookup(key);
    if (args.empty()) return fmt;

    std::string result;
    result.reserve(fmt.size() + 64);
    size_t arg_idx = 0;
    for (size_t i = 0; i < fmt.size(); ++i) {
        if (fmt[i] == '%' && i + 1 < fmt.size() && arg_idx < args.size()) {
            char spec = fmt[i + 1];
            if (spec == 's' || spec == 'd' || spec == 'f') {
                result += args[arg_idx++];
                ++i;
                continue;
            }
        }
        result += fmt[i];
    }
    return result;
}

} // namespace osc::core
