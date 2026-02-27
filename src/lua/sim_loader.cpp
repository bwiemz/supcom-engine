#include "lua/sim_loader.hpp"
#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/sim_bindings.hpp"
#include "sim/sim_state.hpp"
#include "vfs/virtual_file_system.hpp"

#include <spdlog/spdlog.h>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

namespace osc::lua {

Result<void> SimLoader::boot_sim(LuaState& state,
                                  const vfs::VirtualFileSystem& vfs,
                                  sim::SimState& sim) {
    spdlog::info("Booting simulation environment...");

    // Step 1: Register moho bindings (must be before globalInit.lua)
    register_moho_bindings(state, sim);

    // Step 2: Register sim global bindings
    register_sim_bindings(state, sim);

    // Step 3: Load simInit.lua from VFS.
    // simInit.lua internally calls doscript '/lua/globalInit.lua' which:
    //   - loads system modules (config, import, utils, repr, debug, class, etc.)
    //   - iterates moho table and calls ConvertCClassToLuaSimplifiedClass
    // Then it loads GlobalPlatoon/Builder templates, SimHooks, SimSync.
    auto data = vfs.read_file("/lua/simInit.lua");
    if (!data) {
        return Error("simInit.lua not found in VFS");
    }

    auto result =
        state.do_buffer(data->data(), data->size(), "@/lua/simInit.lua");
    if (!result) {
        spdlog::error("simInit.lua failed: {}", result.error().message);
        return Error("simInit.lua failed: " + result.error().message);
    }
    spdlog::info("  Loaded: /lua/simInit.lua");

    // Step 3b: Patch enhancement globals for dual-key compatibility.
    // FA's SimSync.AddUnitEnhancement stores with number key (unit.EntityId),
    // but enhancementcommon.GetEnhancements looks up with tostring(entityID).
    // Also, GetEnhancements returns nil for units with no enhancements, but
    // Unit.OnWorkBegin indexes the result without nil-checking.
    // Fix: wrap AddUnitEnhancement to store with BOTH keys, and wrap
    // GetEnhancements to return {} instead of nil.
    state.do_string(
        "do\n"
        "  -- Override AddUnitEnhancement to store with both number and string keys\n"
        "  function AddUnitEnhancement(unit, enhancement, slot)\n"
        "    if not slot then return end\n"
        "    local id = unit.EntityId\n"
        "    SimUnitEnhancements[id] = SimUnitEnhancements[id] or {}\n"
        "    SimUnitEnhancements[id][slot] = enhancement\n"
        "    SimUnitEnhancements[tostring(id)] = SimUnitEnhancements[id]\n"
        "    local ok, ec = pcall(import, '/lua/enhancementcommon.lua')\n"
        "    if ok and ec and ec.SetEnhancementTable then\n"
        "      ec.SetEnhancementTable(SimUnitEnhancements)\n"
        "    end\n"
        "  end\n"
        "  -- Override RemoveUnitEnhancement to handle dual keys\n"
        "  function RemoveUnitEnhancement(unit, enhancement)\n"
        "    if not unit or unit.Dead then return end\n"
        "    local id = unit.EntityId\n"
        "    local slots = SimUnitEnhancements[id]\n"
        "    if not slots then return end\n"
        "    for k, v in slots do\n"
        "      if v == enhancement then\n"
        "        slots[k] = nil\n"
        "        break\n"
        "      end\n"
        "    end\n"
        "  end\n"
        "  -- Override RemoveAllUnitEnhancements to handle dual keys\n"
        "  function RemoveAllUnitEnhancements(unit)\n"
        "    local id = unit.EntityId\n"
        "    SimUnitEnhancements[id] = nil\n"
        "    SimUnitEnhancements[tostring(id)] = nil\n"
        "  end\n"
        "  -- Patch GetEnhancements to return {} instead of nil\n"
        "  local ok, ec = pcall(import, '/lua/enhancementcommon.lua')\n"
        "  if ok and ec then\n"
        "    local origGet = ec.GetEnhancements\n"
        "    ec.GetEnhancements = function(entityID)\n"
        "      local result = origGet(entityID)\n"
        "      if result then return result end\n"
        "      return SimUnitEnhancements[entityID] or {}\n"
        "    end\n"
        "  end\n"
        "end\n");
    spdlog::info("  Patched enhancement globals for dual-key compatibility");

    // Step 4: Pre-import Unit.lua to make the Unit class available.
    // This must happen after globalInit.lua (which sets up Class, ClassUnit,
    // TrashBag, moho class conversions) but before any CreateUnit calls.
    // Pre-register ArmyBrains as an empty table — simutils.lua captures it
    // at file scope, but SetupSession hasn't run yet to populate it.
    // Use rawset to bypass strict mode on _G
    state.do_string(
        "if rawget(_G, 'ArmyBrains') == nil then rawset(_G, 'ArmyBrains', {}) end");
    // Use pcall so failure doesn't kill the session — we fall back to
    // moho.unit_methods as the unit metatable if this fails.
    // import() returns a module table, so Unit class is at module.Unit,
    // not at _G.Unit.  We store it as _G.__unit_class for CreateUnit.
    auto import_result = state.do_string(
        "local ok, mod = pcall(import, '/lua/sim/Unit.lua')\n"
        "if ok and type(mod) == 'table' and mod.Unit then\n"
        "    rawset(_G, '__unit_class', mod.Unit)\n"
        "else\n"
        "    WARN('Unit.lua import error: ' .. tostring(mod))\n"
        "end");
    if (import_result) {
        lua_pushstring(state.raw(), "__unit_class");
        lua_rawget(state.raw(), LUA_GLOBALSINDEX);
        bool unit_loaded = lua_istable(state.raw(), -1);
        lua_pop(state.raw(), 1);
        spdlog::info("Unit.lua import: {}",
                      unit_loaded ? "Unit class available"
                                  : "Unit class NOT found");
    } else {
        spdlog::warn("Unit.lua import failed: {}",
                      import_result.error().message);
    }

    spdlog::info("Sim environment ready.");
    return {};
}

void SimLoader::run_ticks(sim::SimState& sim, u32 count) {
    for (u32 i = 0; i < count; i++) {
        sim.tick();
    }
}

} // namespace osc::lua
