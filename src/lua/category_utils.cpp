#include "lua/category_utils.hpp"

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

#include <utility>

namespace osc::lua {

CategoryMatcher::CategoryMatcher(lua_State* L, int cat_idx) {
    root_ = compile(L, cat_idx, 0);
}

int CategoryMatcher::add(Kind kind, std::string name) {
    Node node;
    node.kind = kind;
    if (kind == Kind::Name || kind == Kind::AllUnits) {
        node.id = sim::CategoryIds::find(name);
        node.name = std::move(name);
    }
    nodes_.push_back(std::move(node));
    return static_cast<int>(nodes_.size()) - 1;
}

int CategoryMatcher::compile(lua_State* L, int idx, int depth) {
    if (depth > 16) return add(Kind::Never); // guard against pathological nesting

    // Normalise to absolute index before any stack manipulation
    if (idx < 0) idx = lua_gettop(L) + idx + 1;

    if (!lua_istable(L, idx)) return add(Kind::Never);

    // 1. Simple category: { __name = "COMMAND" }
    lua_pushstring(L, "__name");
    lua_rawget(L, idx);
    if (lua_isstring(L, -1)) {
        std::string name = lua_tostring(L, -1);
        lua_pop(L, 1);
        // Every entity with categories but a projectile (whose set holds
        // ALLPROJECTILES).
        if (name == "ALLUNITS") return add(Kind::AllUnits, "ALLPROJECTILES");
        return add(Kind::Name, std::move(name));
    }
    lua_pop(L, 1);

    // 2. Compound category: { __op, __left, __right }
    lua_pushstring(L, "__op");
    lua_rawget(L, idx);
    if (!lua_isstring(L, -1)) {
        lua_pop(L, 1);
        return add(Kind::Never);
    }
    const std::string op = lua_tostring(L, -1);
    lua_pop(L, 1);
    const Kind kind = op == "union"          ? Kind::Union
                      : op == "intersection" ? Kind::Intersection
                      : op == "difference"   ? Kind::Difference
                                             : Kind::Never;
    if (kind == Kind::Never) return add(Kind::Never);

    lua_pushstring(L, "__left");
    lua_rawget(L, idx);
    const int left_idx = lua_gettop(L);
    lua_pushstring(L, "__right");
    lua_rawget(L, idx);
    const int right_idx = lua_gettop(L);

    const int self = add(kind);
    const int left = compile(L, left_idx, depth + 1);
    const int right = compile(L, right_idx, depth + 1);
    nodes_[static_cast<size_t>(self)].left = left; // compile grew nodes_
    nodes_[static_cast<size_t>(self)].right = right;
    lua_pop(L, 2); // pop left + right
    return self;
}

template <typename Has> bool CategoryMatcher::test(int node, const Has& has) const {
    const Node& n = nodes_[static_cast<size_t>(node)];
    switch (n.kind) {
    case Kind::Never: return false;
    case Kind::Name: return has(n);
    case Kind::AllUnits: return !has(n);
    case Kind::Union: return test(n.left, has) || test(n.right, has);
    case Kind::Intersection: return test(n.left, has) && test(n.right, has);
    case Kind::Difference: return test(n.left, has) && !test(n.right, has);
    }
    return false;
}

bool CategoryMatcher::matches(const sim::CategoryBits& cats) const {
    return test(root_, [&](const Node& n) { return n.id && cats.test(*n.id); });
}

bool CategoryMatcher::matches(const std::unordered_set<std::string>& cats) const {
    return test(root_, [&](const Node& n) { return cats.count(n.name) > 0; });
}

bool unit_matches_category(lua_State* L, int cat_idx,
                           const std::unordered_set<std::string>& unit_cats) {
    return CategoryMatcher(L, cat_idx).matches(unit_cats);
}

bool categories_match(lua_State* L, int cat_idx,
                      const std::unordered_set<std::string>& cats) {
    return CategoryMatcher(L, cat_idx).matches(cats);
}

} // namespace osc::lua
