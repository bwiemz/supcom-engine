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

    /// Import the sim's script classes the engine builds objects from
    /// (Unit, Platoon). After SetupSession, as Moho's first import of them
    /// is: modules capture game globals at file scope (FAF's SimUtils keeps
    /// `local ArmyBrains = ArmyBrains`), and SetupSession makes those
    /// globals anew (ArmyBrains, Scenario).
    static void import_script_classes(LuaState& state);

    /// Run N simulation ticks.
    void run_ticks(sim::SimState& sim, u32 count);
};

} // namespace osc::lua
