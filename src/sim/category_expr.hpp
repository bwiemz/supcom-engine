#pragma once

#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

struct lua_State;

namespace osc::sim {

/// An entity category expression, compiled so the sim can test units
/// against it without Lua: a name, ALLUNITS, or a union, intersection or
/// difference of two expressions. An empty expression matches nothing.
/// ALLUNITS matches every set but a projectile's (which holds
/// ALLPROJECTILES).
///
/// Moho copies categories when a script hands them over (FAF's
/// SetWeaponPriorities reuses and clears its table right after calling
/// SetTargetingPriorities), so compiling at that moment matches it.
class CategoryExpr {
public:
    enum class Op : unsigned char { None, All, Name, Union, Intersection, Difference };

    CategoryExpr() = default;
    static CategoryExpr all();
    static CategoryExpr name(std::string category);
    static CategoryExpr combine(Op op, CategoryExpr left, CategoryExpr right);

    bool empty() const { return op_ == Op::None; }
    bool matches(const std::unordered_set<std::string>& categories) const;

private:
    Op op_ = Op::None;
    std::string name_;
    std::vector<CategoryExpr> operands_; // two, for the set operations
};

/// Compile the Lua category at `index` ({__name = ...} or
/// {__op, __left, __right}, as ParseEntityCategory and `categories` make).
/// Anything else compiles to an empty expression. The stack is unchanged.
CategoryExpr compile_category(lua_State* L, int index);

/// A blueprint's category list, as in TargetRestrictDisallow:
/// "UNTARGETABLE, AIR" or "TACTICAL MISSILE". Commas separate alternatives,
/// spaces join the words of one (an intersection); case is ignored.
CategoryExpr parse_category_list(std::string_view text);

} // namespace osc::sim
