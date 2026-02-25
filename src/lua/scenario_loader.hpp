#pragma once

#include "core/result.hpp"
#include "core/types.hpp"

#include <string>
#include <vector>

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

/// Metadata extracted from a *_scenario.lua file.
struct ScenarioMetadata {
    std::string name;
    std::string type;              // "skirmish", "campaign", etc.
    u32 map_width = 0;
    u32 map_height = 0;
    std::string scmap_path;        // from ScenarioInfo.map
    std::string save_path;         // from ScenarioInfo.save
    std::string script_path;       // from ScenarioInfo.script
    std::vector<std::string> armies;
};

class ScenarioLoader {
public:
    /// Load a *_scenario.lua file, parse metadata, load the .scmap heightmap,
    /// and install the Terrain into SimState.
    Result<ScenarioMetadata> load_scenario(
        LuaState& state,
        const vfs::VirtualFileSystem& vfs,
        const std::string& scenario_path,
        sim::SimState& sim);
};

} // namespace osc::lua
