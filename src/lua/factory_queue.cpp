#include "lua/factory_queue.hpp"
#include "sim/unit.hpp"

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

namespace osc::lua {

void FactoryQueueDisplay::set_current(lua_State* L, sim::Unit* factory) {
    if (!factory) { lua_newtable(L); return; }
    current_factory_id_ = factory->entity_id();
    push_queue_table(L, factory);
}

void FactoryQueueDisplay::peek(lua_State* L, sim::Unit* factory) {
    if (!factory) { lua_newtable(L); return; }
    push_queue_table(L, factory);
}

void FactoryQueueDisplay::push_queue_table(lua_State* L, sim::Unit* factory) {
    lua_newtable(L);
    if (!factory) return;
    const auto queue = factory->factory_queue();
    for (size_t i = 0; i < queue.size(); ++i) {
        lua_newtable(L);
        lua_pushstring(L, "id");
        lua_pushstring(L, queue[i].blueprint_id.c_str());
        lua_rawset(L, -3);
        lua_pushstring(L, "count");
        lua_pushnumber(L, queue[i].count);
        lua_rawset(L, -3);
        lua_pushstring(L, "type");
        lua_pushstring(L, "default");
        lua_rawset(L, -3);
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
}

} // namespace osc::lua
