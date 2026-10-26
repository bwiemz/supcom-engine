#include "lua/user_stand_ins.hpp"

#include "lua/lua_state.hpp"

#include <type_traits>

namespace osc::lua {

namespace {

using namespace stand_in;

constexpr StandInText kInstalledLanguages[] = {"us"};

// clang-format off
constexpr UserStandIn kStandIns[] = {
    // userInit.lua sets __language from the player's options before it runs
    // globalInit.lua, and FAF's sets __installedlanguages from /loc: these
    // two matter only to a state booted without one.
    {"__language",              GlobalString{"us"}},
    {"__installedlanguages",    GlobalStrings{kInstalledLanguages}},
    {"PrefetchSession",         Command{}}, // a background load ahead of a launch
    {"ClearFrame",              Command{}},
};
// clang-format on

/// Every function stand-in: upvalue 1 is its entry's index in kStandIns.
int answer(lua_State* L) {
    const auto i = static_cast<std::size_t>(lua_tonumber(L, lua_upvalueindex(1)));
    if (const auto* b = std::get_if<ReturnsBool>(&kStandIns[i].answer)) {
        lua_pushboolean(L, b->value ? 1 : 0);
        return 1;
    }
    return 0; // a Command
}

} // namespace

std::span<const UserStandIn> user_stand_ins() {
    return kStandIns;
}

void register_user_stand_ins(LuaState& state) {
    lua_State* L = state.raw();
    for (std::size_t i = 0; i < std::size(kStandIns); ++i) {
        const UserStandIn& s = kStandIns[i];
        lua_pushstring(L, s.name.c_str());
        std::visit(
            [&](const auto& what) {
                using T = std::decay_t<decltype(what)>;
                if constexpr (std::is_same_v<T, GlobalString>) {
                    lua_pushstring(L, what.value.c_str());
                } else if constexpr (std::is_same_v<T, GlobalStrings>) {
                    lua_newtable(L);
                    int n = 0;
                    for (const StandInText& text : what.values) {
                        lua_pushstring(L, text.c_str());
                        lua_rawseti(L, -2, ++n);
                    }
                } else {
                    lua_pushnumber(L, static_cast<lua_Number>(i));
                    lua_pushcclosure(L, answer, 1);
                }
            },
            s.answer);
        lua_rawset(L, LUA_GLOBALSINDEX);
    }
}

bool is_user_stand_in(lua_CFunction f) {
    return f == answer;
}

} // namespace osc::lua
