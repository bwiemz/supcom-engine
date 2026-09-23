#include "sim/category_expr.hpp"

extern "C" {
#include <lua.h>
}

#include <cctype>
#include <utility>

namespace osc::sim {

CategoryExpr CategoryExpr::all() {
    CategoryExpr e;
    e.op_ = Op::All;
    return e;
}

CategoryExpr CategoryExpr::name(std::string category) {
    if (category == "ALLUNITS") return all();
    CategoryExpr e;
    e.op_ = Op::Name;
    e.name_ = std::move(category);
    return e;
}

CategoryExpr CategoryExpr::combine(Op op, CategoryExpr left, CategoryExpr right) {
    CategoryExpr e;
    e.op_ = op;
    e.operands_.push_back(std::move(left));
    e.operands_.push_back(std::move(right));
    return e;
}

bool CategoryExpr::matches(const std::unordered_set<std::string>& categories) const {
    switch (op_) {
    case Op::None: return false;
    case Op::All: return true;
    case Op::Name: return categories.count(name_) > 0;
    case Op::Union: return operands_[0].matches(categories) || operands_[1].matches(categories);
    case Op::Intersection:
        return operands_[0].matches(categories) && operands_[1].matches(categories);
    case Op::Difference:
        return operands_[0].matches(categories) && !operands_[1].matches(categories);
    }
    return false;
}

namespace {

CategoryExpr compile(lua_State* L, int index, int depth) {
    if (depth > 16 || !lua_istable(L, index)) return {};
    lua_pushstring(L, "__name");
    lua_rawget(L, index);
    if (lua_type(L, -1) == LUA_TSTRING) {
        std::string category = lua_tostring(L, -1);
        lua_pop(L, 1);
        return CategoryExpr::name(std::move(category));
    }
    lua_pop(L, 1);

    lua_pushstring(L, "__op");
    lua_rawget(L, index);
    const std::string op = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);
    CategoryExpr::Op kind = CategoryExpr::Op::None;
    if (op == "union") kind = CategoryExpr::Op::Union;
    else if (op == "intersection") kind = CategoryExpr::Op::Intersection;
    else if (op == "difference") kind = CategoryExpr::Op::Difference;
    else return {};

    lua_pushstring(L, "__left");
    lua_rawget(L, index);
    CategoryExpr left = compile(L, lua_gettop(L), depth + 1);
    lua_pop(L, 1);
    lua_pushstring(L, "__right");
    lua_rawget(L, index);
    CategoryExpr right = compile(L, lua_gettop(L), depth + 1);
    lua_pop(L, 1);
    return CategoryExpr::combine(kind, std::move(left), std::move(right));
}

} // namespace

CategoryExpr compile_category(lua_State* L, int index) {
    if (index < 0) index = lua_gettop(L) + index + 1;
    return compile(L, index, 0);
}

CategoryExpr parse_category_list(std::string_view text) {
    CategoryExpr result;
    CategoryExpr term;
    std::string word;
    const auto end_word = [&] {
        if (word.empty()) return;
        CategoryExpr named = CategoryExpr::name(std::move(word));
        word.clear();
        term = term.empty() ? std::move(named)
                            : CategoryExpr::combine(CategoryExpr::Op::Intersection, std::move(term),
                                                    std::move(named));
    };
    const auto end_term = [&] {
        end_word();
        if (term.empty()) return;
        result = result.empty() ? std::move(term)
                                : CategoryExpr::combine(CategoryExpr::Op::Union, std::move(result),
                                                        std::move(term));
        term = {};
    };
    for (const char c : text) {
        if (c == ',') end_term();
        else if (std::isspace(static_cast<unsigned char>(c))) end_word();
        else word += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    end_term();
    return result;
}

} // namespace osc::sim
