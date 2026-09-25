#pragma once
#include "core/types.hpp"
#include "sim/category_set.hpp"

#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

struct lua_State;

namespace osc::lua {

/// A Lua category expression, compiled once for a query that tests many
/// units. The table at `cat_idx` may be a simple {__name="FOO"} or a
/// compound tree with {__op="union|intersection|difference", __left,
/// __right}; testing a unit then costs a few bit tests, not a walk over Lua
/// tables and a string hash per name. Compile it where the query starts:
/// it reads the table as it is then.
class CategoryMatcher {
public:
    CategoryMatcher(lua_State* L, int cat_idx);

    /// A unit's categories (sim::Unit::category_bits).
    bool matches(const sim::CategoryBits& cats) const;
    /// A set of category names (a blueprint's, see
    /// sim::collect_blueprint_categories, or a projectile's).
    bool matches(const std::unordered_set<std::string>& cats) const;

private:
    enum class Kind : u8 { Never, Name, AllUnits, Union, Intersection, Difference };
    struct Node {
        Kind kind = Kind::Never;
        std::string name;      // Name: the category; AllUnits: ALLPROJECTILES
        std::optional<u32> id; // its CategoryIds id, if anyone has it
        int left = -1;
        int right = -1;
    };

    int compile(lua_State* L, int idx, int depth);
    int add(Kind kind, std::string name = {});
    template <typename Has> bool test(int node, const Has& has) const;

    std::vector<Node> nodes_;
    int root_ = -1;
};

/// Check whether a unit's categories match a Lua category table (compiled
/// for this one test: in a loop over units, compile a CategoryMatcher once).
bool unit_matches_category(lua_State* L, int cat_idx,
                           const std::unordered_set<std::string>& unit_cats);
/// Check whether a set of category strings (see sim::collect_blueprint_categories)
/// matches a Lua category table.  Same logic as above but takes a raw set.
/// This overload is for blueprint-level queries where no C++ Unit exists.
bool categories_match(lua_State* L, int cat_idx,
                      const std::unordered_set<std::string>& cats);

} // namespace osc::lua
