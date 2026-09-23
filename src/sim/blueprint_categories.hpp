#pragma once

#include <string>
#include <unordered_set>

struct lua_State;

namespace osc::sim {

/// Add the category names of the blueprint table at `bp_index` to `out`.
///
/// FAF's blueprint loader precomputes `CategoriesHash` ({NAME = true}); retail
/// FA's does not, and the engine reads the plain `Categories` list
/// ({'NAME', ...}). Both are read and unioned, so either data set works and
/// any derived hash-only entries are kept. Non-string entries are ignored.
/// The Lua stack is left balanced.
void collect_blueprint_categories(lua_State* L, int bp_index,
                                  std::unordered_set<std::string>& out);

} // namespace osc::sim
