#pragma once

struct lua_State;

namespace osc::core {

/// Push onto `to` a deep copy of the value at `from_idx` in `from`. Plain
/// data only -- nil, booleans, numbers, strings and tables of them; other
/// values (functions, userdata, threads) and entries keyed by them are
/// dropped: what crosses between Lua states (Moho's sim sync, preferences)
/// is data, not code. A table reached twice (shared or cyclic) is copied
/// once and shared in the copy too.
void copy_lua_value(lua_State* from, int from_idx, lua_State* to);

} // namespace osc::core
