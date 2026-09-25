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

/// Whether a unit of blueprint `builder_bp` can build `target_bp`: an entry
/// of its Economy.BuildableCategory (a list, or one string) names the
/// blueprint, or all of the entry's tokens are among the target's
/// categories (unit:CanBuild; M206h's factory assist). Reads __blueprints;
/// the Lua stack is left balanced.
bool blueprint_can_build(lua_State* L, const std::string& builder_bp, const std::string& target_bp);

} // namespace osc::sim
