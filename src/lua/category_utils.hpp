#pragma once

#include <string>
#include <unordered_set>

struct lua_State;

namespace osc::lua {

/// Check whether a unit's categories match a Lua category table.
/// The category table at `cat_idx` may be a simple {__name="FOO"} or a
/// compound tree with {__op="union|intersection|difference", __left, __right}.
bool unit_matches_category(lua_State* L, int cat_idx,
                           const std::unordered_set<std::string>& unit_cats);

/// Check whether a set of category strings (from a blueprint's CategoriesHash)
/// matches a Lua category table.  Same logic as above but takes a raw set.
/// This overload is for blueprint-level queries where no C++ Unit exists.
bool categories_match(lua_State* L, int cat_idx,
                      const std::unordered_set<std::string>& cats);

} // namespace osc::lua
