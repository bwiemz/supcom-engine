// Compiled category matching (M224a) answers exactly as the tree walk it
// replaced: the old walk is kept here as the reference, and random
// expressions over random category sets must agree on every one.

#include <catch2/catch_test_macros.hpp>

#include "lua/category_utils.hpp"
#include "sim/category_set.hpp"
#include "sim/manipulator.hpp"
#include "sim/sim_random.hpp"
#include "sim/unit.hpp"

extern "C" {
#include <lua.h>
}

#include <array>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace {

struct LuaGuard {
    lua_State* L = lua_open();
    ~LuaGuard() { lua_close(L); }
};

/// The walk the matcher replaced, as it was.
bool reference_match(lua_State* L, int cat_idx, const std::unordered_set<std::string>& cats,
                     int depth) {
    if (depth > 16) return false;
    if (cat_idx < 0) cat_idx = lua_gettop(L) + cat_idx + 1;
    if (!lua_istable(L, cat_idx)) return false;
    lua_pushstring(L, "__name");
    lua_rawget(L, cat_idx);
    if (lua_isstring(L, -1)) {
        std::string name = lua_tostring(L, -1);
        lua_pop(L, 1);
        if (name == "ALLUNITS") return cats.count("ALLPROJECTILES") == 0;
        return cats.count(name) > 0;
    }
    lua_pop(L, 1);
    lua_pushstring(L, "__op");
    lua_rawget(L, cat_idx);
    if (!lua_isstring(L, -1)) {
        lua_pop(L, 1);
        return false;
    }
    std::string op = lua_tostring(L, -1);
    lua_pop(L, 1);
    lua_pushstring(L, "__left");
    lua_rawget(L, cat_idx);
    const int left_idx = lua_gettop(L);
    lua_pushstring(L, "__right");
    lua_rawget(L, cat_idx);
    const int right_idx = lua_gettop(L);
    bool result = false;
    if (op == "union") {
        result = reference_match(L, left_idx, cats, depth + 1) ||
                 reference_match(L, right_idx, cats, depth + 1);
    } else if (op == "intersection") {
        result = reference_match(L, left_idx, cats, depth + 1) &&
                 reference_match(L, right_idx, cats, depth + 1);
    } else if (op == "difference") {
        result = reference_match(L, left_idx, cats, depth + 1) &&
                 !reference_match(L, right_idx, cats, depth + 1);
    }
    lua_pop(L, 2);
    return result;
}

constexpr std::array<std::string_view, 7> kNames = {
    "LAND", "AIR", "TECH2", "MOBILE", "ALLUNITS", "ALLPROJECTILES", "NEVER_SEEN_M224A"};

void push_name(lua_State* L, std::string_view name) {
    lua_newtable(L);
    lua_pushstring(L, "__name");
    lua_pushlstring(L, name.data(), name.size());
    lua_rawset(L, -3);
}

/// Push a random category expression, as FA's category operators build them
/// (`*` intersection, `+` union, `-` difference), with the odd malformed
/// node: an unknown operator, a missing or non-table operand, a number.
void push_random(lua_State* L, osc::sim::SimRandom& rng, int depth) {
    const auto pick = [&] { return static_cast<int>(rng.next_int(0, 99)); };
    const int roll = pick();
    if (depth >= 5 || roll < 35) {
        push_name(L, kNames[static_cast<size_t>(pick()) % kNames.size()]);
        return;
    }
    if (roll < 38) { // a numeric name: lua_isstring accepts it
        lua_newtable(L);
        lua_pushstring(L, "__name");
        lua_pushnumber(L, 7);
        lua_rawset(L, -3);
        return;
    }
    if (roll < 40) {
        lua_pushnumber(L, 3); // not a table
        return;
    }
    static const char* kOps[] = {"union", "intersection", "difference", "xor"};
    lua_newtable(L);
    lua_pushstring(L, "__op");
    lua_pushstring(L, kOps[roll < 42 ? 3 : pick() % 3]);
    lua_rawset(L, -3);
    lua_pushstring(L, "__left");
    push_random(L, rng, depth + 1);
    lua_rawset(L, -3);
    if (roll != 50) { // a missing right operand now and then
        lua_pushstring(L, "__right");
        push_random(L, rng, depth + 1);
        lua_rawset(L, -3);
    }
}

} // namespace

TEST_CASE("Compiled category matching answers as the tree walk did", "[category]") {
    LuaGuard g;
    lua_State* L = g.L;
    // Some names have ids (some unit had them), one never has.
    for (const auto& name : kNames)
        if (name != "NEVER_SEEN_M224A") osc::sim::CategoryIds::intern(name);

    osc::sim::SimRandom rng(20260925);
    int matched = 0;
    int compared = 0;
    for (int expr = 0; expr < 400; ++expr) {
        const int top = lua_gettop(L);
        push_random(L, rng, 0);
        const osc::lua::CategoryMatcher matcher(L, -1);
        REQUIRE(lua_gettop(L) == top + 1); // compiling leaves the stack as it was
        for (int set = 0; set < 32; ++set) {
            std::unordered_set<std::string> cats;
            osc::sim::CategoryBits bits;
            for (const auto& name : kNames) {
                if (name == "ALLUNITS" || name == "NEVER_SEEN_M224A") continue;
                if (rng.next_int(0, 1) != 0) {
                    cats.emplace(name);
                    bits.set(osc::sim::CategoryIds::intern(name));
                }
            }
            const bool expected = reference_match(L, -1, cats, 0);
            INFO("expression " << expr << ", set " << set);
            REQUIRE(matcher.matches(cats) == expected);
            REQUIRE(matcher.matches(bits) == expected);
            ++compared;
            if (expected) ++matched;
        }
        lua_settop(L, top);
    }
    // The expressions match some sets and not others.
    CHECK(matched > compared / 10);
    CHECK(matched < compared * 9 / 10);
}

TEST_CASE("Category matching stops at 16 levels, as the tree walk did", "[category]") {
    LuaGuard g;
    lua_State* L = g.L;
    osc::sim::CategoryIds::intern("LAND");
    const std::unordered_set<std::string> cats = {"LAND"};
    osc::sim::CategoryBits bits;
    bits.set(osc::sim::CategoryIds::intern("LAND"));
    for (int levels : {15, 16, 17, 20}) {
        // LAND * LAND * ... * LAND, `levels` intersections deep
        push_name(L, "LAND");
        for (int i = 0; i < levels; ++i) {
            lua_newtable(L);
            lua_pushstring(L, "__op");
            lua_pushstring(L, "intersection");
            lua_rawset(L, -3);
            lua_pushstring(L, "__left");
            lua_pushvalue(L, -3);
            lua_rawset(L, -3);
            lua_pushstring(L, "__right");
            push_name(L, "LAND");
            lua_rawset(L, -3);
            lua_remove(L, -2);
        }
        const bool expected = reference_match(L, -1, cats, 0);
        const osc::lua::CategoryMatcher matcher(L, -1);
        INFO(levels << " levels");
        CHECK(matcher.matches(cats) == expected);
        CHECK(matcher.matches(bits) == expected);
        CHECK(expected == (levels <= 16));
        lua_pop(L, 1);
    }
}

TEST_CASE("Category ids are stable, and bits hold only what was set", "[category]") {
    const auto a = osc::sim::CategoryIds::intern("M224A_FIRST");
    CHECK(osc::sim::CategoryIds::intern("M224A_FIRST") == a);
    CHECK(osc::sim::CategoryIds::find("M224A_FIRST") == a);
    CHECK_FALSE(osc::sim::CategoryIds::find("M224A_NOBODY").has_value());
    osc::sim::CategoryBits bits;
    bits.set(200);
    CHECK(bits.test(200));
    CHECK_FALSE(bits.test(199));
    CHECK_FALSE(bits.test(5000)); // past its words
}

TEST_CASE("A unit's category bits follow its categories", "[category]") {
    LuaGuard g;
    lua_State* L = g.L;
    osc::sim::Unit unit;
    unit.add_category("M224A_TANK");
    unit.add_category("LAND");
    push_name(L, "M224A_TANK");
    const osc::lua::CategoryMatcher tank(L, -1);
    push_name(L, "AIR");
    const osc::lua::CategoryMatcher air(L, -1);
    lua_pop(L, 2);
    CHECK(tank.matches(unit.category_bits()));
    CHECK(tank.matches(unit.categories()));
    CHECK_FALSE(air.matches(unit.category_bits()));
}
