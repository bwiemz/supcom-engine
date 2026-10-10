#pragma once

#include "core/types.hpp"

struct lua_State;

namespace osc::sim {

class Projectile;

/// Give a registered projectile its Lua object, as Moho does for every
/// projectile, however it was made: an instance of its blueprint's script
/// class (ScriptModule/ScriptClass, else <dir>/<id>_script.lua's TypeClass),
/// else the generic /lua/sim/Projectile.lua class, else the bare
/// moho.projectile_methods. Then runs its OnCreate(inWater). Leaves the
/// object on the stack when `push`. `source`: what made it, its launcher
/// when 0.
void create_projectile_object(lua_State* L, Projectile& proj, bool in_water, bool push,
                              u32 source = 0);

} // namespace osc::sim
