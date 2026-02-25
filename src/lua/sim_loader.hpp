#pragma once

#include "core/result.hpp"
#include "core/types.hpp"

namespace osc::lua {
class LuaState;
}
namespace osc::vfs {
class VirtualFileSystem;
}
namespace osc::blueprints {
class BlueprintStore;
}
namespace osc::sim {
class SimState;
}

namespace osc::lua {

class SimLoader {
public:
    /// Boot the simulation Lua environment:
    /// 1. Register moho bindings + sim global bindings
    /// 2. Load simInit.lua (which internally loads globalInit.lua)
    Result<void> boot_sim(LuaState& state,
                          const vfs::VirtualFileSystem& vfs,
                          sim::SimState& sim);

    /// Run N simulation ticks.
    void run_ticks(sim::SimState& sim, u32 count);
};

} // namespace osc::lua
