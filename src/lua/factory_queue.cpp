#include "lua/factory_queue.hpp"
#include "core/game_state.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"

#include <algorithm>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

namespace osc::lua {

void FactoryQueueDisplay::set_current(lua_State* L, sim::Unit* factory) {
    static const sim::CategoryName kShowQueue{"SHOWQUEUE"};
    clear();
    if (factory && factory->has_category(kShowQueue)) {
        current_factory_id_ = factory->entity_id();
        shown_ = factory->factory_queue();
    }
    if (shown_.empty()) {
        lua_pushnil(L);
        return;
    }
    push_queue_table(L, shown_);
}

void FactoryQueueDisplay::peek(lua_State* L, sim::Unit* factory) {
    if (!factory) { lua_newtable(L); return; }
    push_queue_table(L, factory->factory_queue());
}

void FactoryQueueDisplay::report_change(lua_State* L, sim::SimState& sim) {
    if (current_factory_id_ == 0 || !L) {
        return;
    }
    std::vector<sim::BuildQueueEntry> queue;
    if (auto* e = sim.entity_registry().find(current_factory_id_);
        e && e->is_unit() && !e->destroyed()) {
        queue = static_cast<sim::Unit*>(e)->factory_queue();
    }
    const bool same = queue.size() == shown_.size() &&
                      std::equal(queue.begin(), queue.end(), shown_.begin(),
                                 [](const sim::BuildQueueEntry& a, const sim::BuildQueueEntry& b) {
                                     return a.blueprint_id == b.blueprint_id && a.count == b.count;
                                 });
    if (same) {
        return;
    }
    shown_ = std::move(queue);
    push_queue_table(L, shown_);
    core::call_ui_callback(L, core::kGameMainModule, "OnQueueChanged", 1);
}

void FactoryQueueDisplay::push_queue_table(lua_State* L,
                                           const std::vector<sim::BuildQueueEntry>& queue) {
    lua_newtable(L);
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
