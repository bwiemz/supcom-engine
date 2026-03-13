#include "ui/keymap.hpp"

#include <GLFW/glfw3.h>
#include <spdlog/spdlog.h>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

namespace osc::ui {

void KeyMapRegistry::add(lua_State* L, int table_idx) {
    // Normalize negative index
    if (table_idx < 0) table_idx = lua_gettop(L) + table_idx + 1;

    KeyMapTable kmt;
    kmt.table_ptr = lua_topointer(L, table_idx);

    // Store a registry ref to the original table
    lua_pushvalue(L, table_idx);
    kmt.table_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    // Iterate the Lua table: keys are key name strings, values are either
    // functions directly or tables with an "action" function field.
    lua_pushnil(L);
    while (lua_next(L, table_idx) != 0) {
        // key at -2, value at -1
        if (lua_type(L, -2) == LUA_TSTRING) {
            const char* key_str = lua_tostring(L, -2);
            std::string key_name(key_str);

            KeyBinding binding;
            binding.key_name = key_name;

            if (lua_isfunction(L, -1)) {
                // Value is a function directly
                lua_pushvalue(L, -1);
                binding.action_ref = luaL_ref(L, LUA_REGISTRYINDEX);
                binding.action_name = key_name;
            } else if (lua_istable(L, -1)) {
                // Value is a table with an "action" field
                lua_pushstring(L, "action");
                lua_rawget(L, -2);
                if (lua_isfunction(L, -1)) {
                    binding.action_ref = luaL_ref(L, LUA_REGISTRYINDEX);
                    binding.action_name = key_name;
                } else {
                    // Try string action name for debug info
                    if (lua_type(L, -1) == LUA_TSTRING) {
                        binding.action_name = lua_tostring(L, -1);
                    }
                    lua_pop(L, 1);
                }
            }

            if (binding.action_ref >= 0) {
                kmt.bindings.push_back(std::move(binding));
            }
        }

        lua_pop(L, 1); // pop value, keep key for lua_next
    }

    spdlog::debug("KeyMapRegistry: added table with {} bindings", kmt.bindings.size());
    tables_.push_back(std::move(kmt));
}

void KeyMapRegistry::remove_by_ref(lua_State* L, int table_idx) {
    // Normalize negative index
    if (table_idx < 0) table_idx = lua_gettop(L) + table_idx + 1;

    const void* ptr = lua_topointer(L, table_idx);

    for (auto it = tables_.begin(); it != tables_.end(); ++it) {
        if (it->table_ptr == ptr) {
            // Unref all action bindings
            for (auto& b : it->bindings) {
                if (b.action_ref >= 0) {
                    luaL_unref(L, LUA_REGISTRYINDEX, b.action_ref);
                }
            }
            // Unref the table itself
            if (it->table_ref >= 0) {
                luaL_unref(L, LUA_REGISTRYINDEX, it->table_ref);
            }
            spdlog::debug("KeyMapRegistry: removed table with {} bindings",
                          it->bindings.size());
            tables_.erase(it);
            return;
        }
    }

    spdlog::warn("KeyMapRegistry: remove_by_ref called but table not found");
}

int KeyMapRegistry::find_action(const std::string& key_name) const {
    // Search last-to-first (later-added tables take priority)
    for (int i = static_cast<int>(tables_.size()) - 1; i >= 0; --i) {
        for (const auto& b : tables_[i].bindings) {
            if (b.key_name == key_name) {
                return b.action_ref;
            }
        }
    }
    return LUA_NOREF;
}

bool KeyMapRegistry::dispatch(lua_State* L, const std::string& key_name) {
    int ref = find_action(key_name);
    if (ref < 0) return false;

    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        return false;
    }

    if (lua_pcall(L, 0, 0, 0) != 0) {
        spdlog::warn("KeyMapRegistry: dispatch error for '{}': {}",
                     key_name, lua_tostring(L, -1));
        lua_pop(L, 1);
        // Key was consumed (binding existed) even though handler errored
    }

    return true;
}

void KeyMapRegistry::clear(lua_State* L) {
    for (auto& kmt : tables_) {
        for (auto& b : kmt.bindings) {
            if (b.action_ref >= 0) {
                luaL_unref(L, LUA_REGISTRYINDEX, b.action_ref);
            }
        }
        if (kmt.table_ref >= 0) {
            luaL_unref(L, LUA_REGISTRYINDEX, kmt.table_ref);
        }
    }
    tables_.clear();
    spdlog::debug("KeyMapRegistry: cleared all tables");
}

std::string KeyMapRegistry::glfw_to_key_name(int glfw_key, int mods) {
    std::string name;

    // Modifier prefixes
    if (mods & GLFW_MOD_CONTROL) name += "Ctrl-";
    if (mods & GLFW_MOD_SHIFT)   name += "Shift-";
    if (mods & GLFW_MOD_ALT)     name += "Alt-";

    // Map GLFW key codes to FA-style key name strings
    // Letters A-Z
    if (glfw_key >= GLFW_KEY_A && glfw_key <= GLFW_KEY_Z) {
        name += static_cast<char>('A' + (glfw_key - GLFW_KEY_A));
    }
    // Digits 0-9
    else if (glfw_key >= GLFW_KEY_0 && glfw_key <= GLFW_KEY_9) {
        name += static_cast<char>('0' + (glfw_key - GLFW_KEY_0));
    }
    // Function keys F1-F12
    else if (glfw_key >= GLFW_KEY_F1 && glfw_key <= GLFW_KEY_F12) {
        name += "F" + std::to_string(glfw_key - GLFW_KEY_F1 + 1);
    }
    // Named keys
    else {
        switch (glfw_key) {
        case GLFW_KEY_ESCAPE:       name += "Escape"; break;
        case GLFW_KEY_SPACE:        name += "Space"; break;
        case GLFW_KEY_ENTER:        name += "Enter"; break;
        case GLFW_KEY_TAB:          name += "Tab"; break;
        case GLFW_KEY_DELETE:       name += "Delete"; break;
        case GLFW_KEY_BACKSPACE:    name += "Backspace"; break;
        case GLFW_KEY_INSERT:       name += "Insert"; break;
        case GLFW_KEY_HOME:         name += "Home"; break;
        case GLFW_KEY_END:          name += "End"; break;
        case GLFW_KEY_PAGE_UP:      name += "PageUp"; break;
        case GLFW_KEY_PAGE_DOWN:    name += "PageDown"; break;
        case GLFW_KEY_UP:           name += "Up"; break;
        case GLFW_KEY_DOWN:         name += "Down"; break;
        case GLFW_KEY_LEFT:         name += "Left"; break;
        case GLFW_KEY_RIGHT:        name += "Right"; break;
        case GLFW_KEY_PAUSE:        name += "Pause"; break;
        case GLFW_KEY_PRINT_SCREEN: name += "PrintScreen"; break;
        case GLFW_KEY_NUM_LOCK:     name += "NumLock"; break;
        case GLFW_KEY_CAPS_LOCK:    name += "CapsLock"; break;
        case GLFW_KEY_SCROLL_LOCK:  name += "ScrollLock"; break;
        case GLFW_KEY_KP_0:         name += "Numpad0"; break;
        case GLFW_KEY_KP_1:         name += "Numpad1"; break;
        case GLFW_KEY_KP_2:         name += "Numpad2"; break;
        case GLFW_KEY_KP_3:         name += "Numpad3"; break;
        case GLFW_KEY_KP_4:         name += "Numpad4"; break;
        case GLFW_KEY_KP_5:         name += "Numpad5"; break;
        case GLFW_KEY_KP_6:         name += "Numpad6"; break;
        case GLFW_KEY_KP_7:         name += "Numpad7"; break;
        case GLFW_KEY_KP_8:         name += "Numpad8"; break;
        case GLFW_KEY_KP_9:         name += "Numpad9"; break;
        case GLFW_KEY_KP_ADD:       name += "NumpadPlus"; break;
        case GLFW_KEY_KP_SUBTRACT:  name += "NumpadMinus"; break;
        case GLFW_KEY_KP_MULTIPLY:  name += "NumpadStar"; break;
        case GLFW_KEY_KP_DIVIDE:    name += "NumpadSlash"; break;
        case GLFW_KEY_KP_DECIMAL:   name += "NumpadDot"; break;
        case GLFW_KEY_KP_ENTER:     name += "NumpadEnter"; break;
        case GLFW_KEY_MINUS:        name += "Minus"; break;
        case GLFW_KEY_EQUAL:        name += "Equals"; break;
        case GLFW_KEY_LEFT_BRACKET: name += "LBracket"; break;
        case GLFW_KEY_RIGHT_BRACKET:name += "RBracket"; break;
        case GLFW_KEY_SEMICOLON:    name += "Semicolon"; break;
        case GLFW_KEY_APOSTROPHE:   name += "Apostrophe"; break;
        case GLFW_KEY_COMMA:        name += "Comma"; break;
        case GLFW_KEY_PERIOD:       name += "Period"; break;
        case GLFW_KEY_SLASH:        name += "Slash"; break;
        case GLFW_KEY_BACKSLASH:    name += "Backslash"; break;
        case GLFW_KEY_GRAVE_ACCENT: name += "Grave"; break;
        default:
            name += "Key" + std::to_string(glfw_key);
            break;
        }
    }

    return name;
}

} // namespace osc::ui
