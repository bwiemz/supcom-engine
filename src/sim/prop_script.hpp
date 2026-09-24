#pragma once

#include "sim/entity.hpp" // Vector3, Quaternion

#include <string>

struct lua_State;

namespace osc::sim {

class Prop;
class SimState;

/// Give a registered prop its Lua object, as Moho does for every prop --
/// a map's, CreateProp's, a wreck: an instance of its blueprint's script
/// class (ScriptModule/ScriptClass, as trees name /lua/proptree.lua and
/// wrecks /lua/wreckage.lua), else /lua/sim/Prop.lua's Prop, else the bare
/// moho.prop_methods. The prop gets its mesh's bones, then its OnCreate
/// runs. Leaves the object on the stack when `push`.
void create_prop_object(lua_State* L, SimState& sim, Prop& prop, bool push);

/// Register a new prop of blueprint `bp_id` at `position` and give it its
/// object (left on the stack when `push`). CreateProp, CreatePropHPR,
/// CreatePropAtBone and SplitProp all make props this way.
Prop* spawn_prop(lua_State* L, SimState& sim, const std::string& bp_id, const Vector3& position,
                 const Quaternion& orientation, bool push);

/// Every registered prop still without a Lua object (a map's, created before
/// the sim's scripts were loaded) gets one.
void create_map_prop_objects(lua_State* L, SimState& sim);

} // namespace osc::sim
