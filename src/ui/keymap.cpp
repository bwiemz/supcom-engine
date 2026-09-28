#include "ui/keymap.hpp"

#include "ui/key_codes.hpp"

#include <GLFW/glfw3.h>
#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <cctype>
#include <vector>

extern "C" {
#include <lua.h>
}

namespace osc::ui {

namespace {

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

/// gpg::STR_Xtoi: the leading hex digits (after an optional 0x) as a number.
u32 hex_to_u32(std::string_view s) {
    if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s.remove_prefix(2);
    u32 v = 0;
    for (char c : s) {
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;
        v = v * 16 + static_cast<u32>(d);
        if (v > 0xFFFFFF) break; // far out of range already
    }
    return v;
}

/// A table key as text (a string, or a number as Lua writes it).
bool key_text(lua_State* L, int idx, std::string& out) {
    if (lua_type(L, idx) != LUA_TSTRING && lua_type(L, idx) != LUA_TNUMBER) return false;
    lua_pushvalue(L, idx); // lua_tostring converts in place; leave the key alone
    out = lua_tostring(L, -1);
    lua_pop(L, 1);
    return true;
}

} // namespace

KeyMapRegistry::KeyMapRegistry() {
    for (u32 code = 0; code < names_.size(); ++code)
        names_[code] = fmt::format("Unknown{:02X}", code);
}

void KeyMapRegistry::set_key_names(lua_State* L, int table_idx) {
    if (table_idx < 0) table_idx = lua_gettop(L) + table_idx + 1;
    if (!lua_istable(L, table_idx)) {
        spdlog::warn("CUIKeyHandler::SetKeyNameTable wasn't passed a table");
        return;
    }
    std::string key;
    lua_pushnil(L);
    while (lua_next(L, table_idx) != 0) {
        if (key_text(L, -2, key) && lua_type(L, -1) == LUA_TSTRING) {
            const u32 code = hex_to_u32(key);
            if (code > 0xFF)
                spdlog::warn(
                    "CUIKeyHandler::SetKeyNameTable found incorrect key code in key names: {}",
                    key);
            else names_[code] = lua_tostring(L, -1);
        }
        lua_pop(L, 1);
    }
}

i32 KeyMapRegistry::parse(std::string_view spec) const {
    if (spec.empty()) return 0;
    std::vector<std::string_view> tokens;
    size_t pos = 0;
    while (pos <= spec.size()) {
        const size_t dash = spec.find('-', pos);
        const std::string_view token =
            spec.substr(pos, dash == std::string_view::npos ? spec.npos : dash - pos);
        if (!token.empty()) tokens.push_back(token);
        if (dash == std::string_view::npos) break;
        pos = dash + 1;
    }
    if (tokens.empty()) return -1;
    // The last token is the key.
    i32 mask = -1;
    for (u32 code = 0; code < names_.size(); ++code) {
        if (iequals(names_[code], tokens.back())) {
            mask = static_cast<i32>(code);
            break;
        }
    }
    for (size_t i = 0; i + 1 < tokens.size(); ++i) {
        if (iequals(tokens[i], "SHIFT")) mask |= static_cast<i32>(kShift);
        else if (iequals(tokens[i], "CTRL")) mask |= static_cast<i32>(kCtrl);
        else if (iequals(tokens[i], "ALT")) mask |= static_cast<i32>(kAlt);
        else spdlog::warn("Key map contains unrecognized modifier string: {}", tokens[i]);
    }
    return mask;
}

void KeyMapRegistry::add(lua_State* L, int table_idx) {
    if (table_idx < 0) table_idx = lua_gettop(L) + table_idx + 1;
    if (!lua_istable(L, table_idx)) {
        spdlog::warn("CUIKeyHandler::AddKeyMapTable requires a table");
        return;
    }
    std::string key;
    lua_pushnil(L);
    while (lua_next(L, table_idx) != 0) {
        if (key_text(L, -2, key)) {
            const u32 chord = static_cast<u32>(parse(key));
            std::string action;
            bool repeat = false;
            if (lua_istable(L, -1)) {
                lua_pushstring(L, "action");
                lua_gettable(L, -2);
                if (lua_type(L, -1) == LUA_TSTRING || lua_type(L, -1) == LUA_TNUMBER)
                    action = lua_tostring(L, -1);
                lua_pop(L, 1);
                lua_pushstring(L, "keyRepeat");
                lua_gettable(L, -2);
                repeat = lua_toboolean(L, -1) != 0;
                lua_pop(L, 1);
            }
            actions_[chord] = std::move(action);
            if (repeat) repeats_.insert(chord);
        }
        lua_pop(L, 1);
    }
}

void KeyMapRegistry::remove(lua_State* L, int table_idx) {
    if (table_idx < 0) table_idx = lua_gettop(L) + table_idx + 1;
    if (!lua_istable(L, table_idx)) {
        spdlog::warn("CUIKeyHandler::RemoveKeyMapTable requires a table");
        return;
    }
    std::string key;
    lua_pushnil(L);
    while (lua_next(L, table_idx) != 0) {
        if (key_text(L, -2, key)) {
            const u32 chord = static_cast<u32>(parse(key));
            actions_.erase(chord);
            repeats_.erase(chord);
        }
        lua_pop(L, 1);
    }
}

void KeyMapRegistry::clear() {
    actions_.clear();
    repeats_.clear();
}

u32 KeyMapRegistry::chord(i32 glfw_key, i32 glfw_mods) {
    u32 c = static_cast<u32>(windows_key_code(glfw_key));
    if (glfw_mods & GLFW_MOD_SHIFT) c |= kShift;
    if (glfw_mods & GLFW_MOD_CONTROL) c |= kCtrl;
    if (glfw_mods & GLFW_MOD_ALT) c |= kAlt;
    return c;
}

const std::string* KeyMapRegistry::action(u32 chord) const {
    const auto it = actions_.find(chord);
    return it == actions_.end() ? nullptr : &it->second;
}

} // namespace osc::ui
