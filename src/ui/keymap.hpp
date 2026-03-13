#pragma once

#include <string>
#include <vector>

struct lua_State;

namespace osc::ui {

struct KeyBinding {
    std::string key_name;      // e.g., "A", "Shift-G", "Ctrl-1"
    int action_ref = -2;       // Lua registry ref to the action function (LUA_NOREF = -2)
    std::string action_name;   // Debug: action name string
};

struct KeyMapTable {
    int table_ref = -2;  // Lua registry ref — keeps the table alive; freed on removal/clear
    const void* table_ptr = nullptr;  // Lua pointer identity (for removal matching)
    std::vector<KeyBinding> bindings;
};

/// Stack of key map tables. Later-added tables take priority (searched last-to-first).
class KeyMapRegistry {
public:
    void add(lua_State* L, int table_idx);
    void remove_by_ref(lua_State* L, int table_idx);
    int find_action(const std::string& key_name) const;
    bool dispatch(lua_State* L, const std::string& key_name);
    void clear(lua_State* L);
    static std::string glfw_to_key_name(int glfw_key, int mods);

private:
    std::vector<KeyMapTable> tables_;
};

} // namespace osc::ui
