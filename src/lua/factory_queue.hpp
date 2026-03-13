#pragma once

#include "core/types.hpp"

struct lua_State;

namespace osc::sim { class Unit; }

namespace osc::lua {

class FactoryQueueDisplay {
public:
    void set_current(lua_State* L, sim::Unit* factory);
    void peek(lua_State* L, sim::Unit* factory);
    void clear() { current_factory_id_ = 0; }
    void decrease_count(sim::Unit* factory, int index, int count);
    u32 current_factory_id() const { return current_factory_id_; }

private:
    u32 current_factory_id_ = 0;
    static void push_queue_table(lua_State* L, sim::Unit* factory);
};

} // namespace osc::lua
