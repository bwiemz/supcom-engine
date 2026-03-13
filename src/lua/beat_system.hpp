#pragma once

#include <string>
#include <vector>

struct lua_State;

namespace osc::lua {

class BeatFunctionRegistry {
public:
    void add(lua_State* L, int func_idx, const std::string& name = "");
    void remove(lua_State* L, int func_idx);
    void remove_by_name(const std::string& name, lua_State* L);
    void fire_all(lua_State* L);
    void clear(lua_State* L);
    size_t count() const { return entries_.size(); }

private:
    struct Entry {
        int lua_ref = -2; // LUA_NOREF
        std::string name;
    };
    std::vector<Entry> entries_;
};

} // namespace osc::lua
