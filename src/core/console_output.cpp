#include "core/console_output.hpp"

#include <spdlog/spdlog.h>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <cstdio>

namespace osc::core {

namespace {

constexpr const char* kReceivers = "__osc_console_receivers";
constexpr const char* kNextReceiver = "__osc_console_receiver_next";

void push_receivers(lua_State* L) {
    lua_pushstring(L, kReceivers);
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushstring(L, kReceivers);
        lua_pushvalue(L, -2);
        lua_rawset(L, LUA_REGISTRYINDEX);
    }
}

int next_receiver(lua_State* L) {
    lua_pushstring(L, kNextReceiver);
    lua_rawget(L, LUA_REGISTRYINDEX);
    const int next = lua_isnumber(L, -1) ? static_cast<int>(lua_tonumber(L, -1)) : 1;
    lua_pop(L, 1);
    return next;
}

} // namespace

int add_console_receiver(lua_State* L, int index) {
    if (index < 0) {
        index = lua_gettop(L) + index + 1;
    }
    const int handle = next_receiver(L);
    lua_pushstring(L, kNextReceiver);
    lua_pushnumber(L, handle + 1);
    lua_rawset(L, LUA_REGISTRYINDEX);
    push_receivers(L);
    lua_pushvalue(L, index);
    lua_rawseti(L, -2, handle);
    lua_pop(L, 1);
    return handle;
}

void remove_console_receiver(lua_State* L, int handle) {
    push_receivers(L);
    lua_pushnil(L);
    lua_rawseti(L, -2, handle);
    lua_pop(L, 1);
}

void console_print(lua_State* L, std::string_view line) {
    const int last = next_receiver(L) - 1;
    for (int handle = 1; handle <= last; ++handle) {
        push_receivers(L);
        lua_rawgeti(L, -1, handle);
        lua_remove(L, -2);
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 1);
            continue;
        }
        lua_pushlstring(L, line.data(), line.size());
        if (lua_pcall(L, 1, 0, 0) != 0) {
            spdlog::warn("Console output receiver: {}",
                         lua_isstring(L, -1) ? lua_tostring(L, -1) : "error");
            lua_pop(L, 1);
        }
    }
}

void concat_print_args(lua_State* L, unsigned char delimiter,
                       const std::function<void(const std::string&)>& emit) {
    std::string line;
    const auto put = [&](unsigned char c) {
        if (c == '\n') {
            emit(line);
            line.clear();
        } else if (c == '\t') {
            const auto column = static_cast<unsigned char>(line.size());
            line.append((static_cast<unsigned char>(column - 1u) & 7u) + 1u, ' ');
        } else if (c < 0x20) {
            char escaped[8];
            std::snprintf(escaped, sizeof escaped, "\\%03o", c);
            line += escaped;
        } else {
            line.push_back(static_cast<char>(c));
        }
    };
    const int count = lua_gettop(L);
    for (int i = 1; i <= count; ++i) {
        if (i != 1) {
            put(delimiter);
        }
        std::string text;
        if (lua_isstring(L, i)) {
            text = lua_tostring(L, i);
        } else {
            lua_pushstring(L, "tostring");
            lua_rawget(L, LUA_GLOBALSINDEX);
            lua_pushvalue(L, i);
            lua_call(L, 1, 1);
            if (!lua_isstring(L, -1)) {
                luaL_typerror(L, i, "string");
            }
            text = lua_tostring(L, -1);
            lua_pop(L, 1);
        }
        for (const char c : text) {
            put(static_cast<unsigned char>(c));
        }
    }
    if (!line.empty()) {
        emit(line);
    }
}

} // namespace osc::core
