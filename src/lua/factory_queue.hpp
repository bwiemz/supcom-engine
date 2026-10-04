#pragma once

#include "core/types.hpp"
#include "sim/unit.hpp"

#include <vector>

struct lua_State;

namespace osc::sim {
class SimState;
}

namespace osc::lua {

class FactoryQueueDisplay {
public:
    void set_current(lua_State* L, sim::Unit* factory);
    void peek(lua_State* L, sim::Unit* factory);
    void clear() {
        current_factory_id_ = 0;
        shown_.clear();
    }
    u32 current_factory_id() const { return current_factory_id_; }
    /// Gives gamemain.OnQueueChanged the shown unit's queue when it changed,
    /// as Moho does
    void report_change(lua_State* L, sim::SimState& sim);

private:
    u32 current_factory_id_ = 0;
    std::vector<sim::BuildQueueEntry> shown_; ///< the queue the UI last had
    static void push_queue_table(lua_State* L, const std::vector<sim::BuildQueueEntry>& queue);
};

} // namespace osc::lua
