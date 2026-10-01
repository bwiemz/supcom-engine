#pragma once

#include "core/types.hpp"

struct lua_State;

namespace osc::ui {

/// A control's LazyVar (Left, Depth, ...) from its Lua table at `table_idx`:
/// the var's value, a plain number as it is, else 0.
f32 read_lazyvar(lua_State* L, int table_idx, const char* field);

} // namespace osc::ui
