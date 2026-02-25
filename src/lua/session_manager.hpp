#pragma once

#include "core/result.hpp"
#include "core/types.hpp"
#include "lua/scenario_loader.hpp"

#include <algorithm>
#include <string>
#include <vector>

struct lua_State;

namespace osc::lua {
class LuaState;
}
namespace osc::vfs {
class VirtualFileSystem;
}
namespace osc::sim {
class SimState;
}

namespace osc::lua {

class SessionManager {
public:
    /// Run the full session lifecycle:
    /// 1. Populate ScenarioInfo.ArmySetup
    /// 2. Call SetupSession() (loads save/script files)
    /// 3. Extract start positions from Scenario.MasterChain markers
    /// 4. Create army brains (C++ + Lua tables + OnCreateArmyBrain calls)
    /// 5. Call BeginSession()
    Result<void> start_session(LuaState& state,
                               const vfs::VirtualFileSystem& vfs,
                               sim::SimState& sim,
                               const ScenarioMetadata& meta);

    bool session_active() const { return session_active_; }

    /// Mark specific army indices (0-based) as AI (Human=false).
    void set_ai_armies(const std::vector<int>& indices) {
        ai_army_indices_ = indices;
    }

    bool is_ai_army(int index) const {
        return std::find(ai_army_indices_.begin(), ai_army_indices_.end(),
                         index) != ai_army_indices_.end();
    }

private:
    void setup_army_info(lua_State* L, const ScenarioMetadata& meta);
    Result<void> call_setup_session(lua_State* L);
    void extract_start_positions(lua_State* L, sim::SimState& sim);
    Result<void> create_army_brain(lua_State* L, sim::SimState& sim,
                                   i32 index, const std::string& name,
                                   const std::string& nickname);
    Result<void> call_begin_session(lua_State* L);

    bool session_active_ = false;
    std::vector<int> ai_army_indices_;
};

} // namespace osc::lua
