#include "lua/url_bindings.hpp"
#include "lua/lua_state.hpp"

#include <algorithm>
#include <cctype>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace osc::lua {

namespace {

constexpr const char* kOpenerKey = "__osc_url_opener";
constexpr const char* kOpenURLHelp =
    "OpenURL(string) - open the default browser window to the specified URL";

bool same_letters(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) ==
                      std::tolower(static_cast<unsigned char>(y));
           });
}

int l_OpenURL(lua_State* L) {
    const int n = lua_gettop(L);
    if (n != 1) return luaL_error(L, "%s\n  expected %d args, but got %d", kOpenURLHelp, 1, n);
    const char* url = lua_tostring(L, 1);
    if (!url) return luaL_typerror(L, 1, "string");

    lua_pushstring(L, kOpenerKey);
    lua_rawget(L, LUA_REGISTRYINDEX);
    auto* opener = static_cast<UrlOpener*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    if (!opener || !opener->open) return 0;

    const std::string scheme = url_scheme(url);
    if (scheme.empty()) return 0;
    const auto& allowed = opener->protocols;
    if (std::any_of(allowed.begin(), allowed.end(),
                    [&](const std::string& p) { return same_letters(p, scheme); }))
        opener->open(url);
    return 0;
}

} // namespace

std::string url_scheme(std::string_view url) {
    if (std::any_of(url.begin(), url.end(), [](char c) {
            const auto u = static_cast<unsigned char>(c);
            return u <= 0x20 || u == 0x7f;
        }))
        return {};
    const size_t colon = url.find(':');
    if (colon == std::string_view::npos || colon == 0) return {};
    const std::string_view scheme = url.substr(0, colon);
    if (!std::isalpha(static_cast<unsigned char>(scheme[0]))) return {};
    const bool valid = std::all_of(scheme.begin(), scheme.end(), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '+' || c == '-' || c == '.';
    });
    return valid ? std::string(scheme) : std::string();
}

void register_url_bindings(LuaState& state, UrlOpener* opener) {
    lua_State* L = state.raw();
    lua_pushstring(L, kOpenerKey);
    lua_pushlightuserdata(L, opener);
    lua_rawset(L, LUA_REGISTRYINDEX);
    state.register_function("OpenURL", l_OpenURL);
}

} // namespace osc::lua
