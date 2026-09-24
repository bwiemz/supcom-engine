#include <catch2/catch_test_macros.hpp>

#include "sim/category_expr.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <string>
#include <unordered_set>

using osc::sim::CategoryExpr;
using osc::sim::compile_category;
using osc::sim::parse_category_list;

namespace {

using Cats = std::unordered_set<std::string>;

struct LuaGuard {
    lua_State* L = lua_open();
    ~LuaGuard() { lua_close(L); }
};

CategoryExpr compiled(const char* source) {
    LuaGuard g;
    const std::string chunk = std::string("return ") + source;
    REQUIRE(luaL_loadbuffer(g.L, chunk.data(), chunk.size(), "cat") == 0);
    REQUIRE(lua_pcall(g.L, 0, 1, 0) == 0);
    const int top = lua_gettop(g.L);
    CategoryExpr e = compile_category(g.L, -1);
    CHECK(lua_gettop(g.L) == top); // stack balanced
    return e;
}

} // namespace

TEST_CASE("Blueprint category lists: commas are alternatives, spaces intersect", "[category]") {
    const CategoryExpr disallow = parse_category_list("UNTARGETABLE, HOVER");
    CHECK(disallow.matches({"UNTARGETABLE"}));
    CHECK(disallow.matches({"HOVER", "LAND"}));
    CHECK_FALSE(disallow.matches({"LAND", "MOBILE"}));

    const CategoryExpr missiles = parse_category_list("TACTICAL MISSILE");
    CHECK(missiles.matches({"TACTICAL", "MISSILE", "PROJECTILE"}));
    CHECK_FALSE(missiles.matches({"TACTICAL"}));
    CHECK_FALSE(missiles.matches({"MISSILE"}));

    const CategoryExpr naval = parse_category_list("UNTARGETABLE,LAND,STRUCTURE,NAVAL");
    CHECK(naval.matches({"NAVAL"}));
    CHECK(naval.matches({"STRUCTURE"}));
    CHECK_FALSE(naval.matches({"AIR"}));

    CHECK(parse_category_list("tactical,missile").matches({"MISSILE"})); // case ignored
    CHECK(parse_category_list("ALLUNITS").matches({}));
}

TEST_CASE("An empty category list matches nothing", "[category]") {
    CHECK(parse_category_list("").empty());
    CHECK(parse_category_list(" , ,").empty());
    CHECK_FALSE(CategoryExpr{}.matches({"LAND"}));
}

TEST_CASE("Lua categories compile to the same test", "[category]") {
    const CategoryExpr air_not_high = compiled(
        "{__op = 'difference', __left = {__name = 'AIR'}, __right = {__name = 'HIGHALTAIR'}}");
    CHECK(air_not_high.matches({"AIR", "MOBILE"}));
    CHECK_FALSE(air_not_high.matches({"AIR", "HIGHALTAIR"}));

    const CategoryExpr t3_mobile =
        compiled("{__op = 'intersection', __left = {__name = 'TECH3'}, "
                 "__right = {__op = 'union', __left = {__name = 'MOBILE'}, "
                 "__right = {__name = 'NAVAL'}}}");
    CHECK(t3_mobile.matches({"TECH3", "NAVAL"}));
    CHECK_FALSE(t3_mobile.matches({"TECH2", "MOBILE"}));

    CHECK(compiled("{__name = 'ALLUNITS'}").matches({}));
    CHECK(compiled("{}").empty());
    CHECK(compiled("{__op = 'xor', __left = {__name = 'A'}, __right = {__name = 'B'}}").empty());
    CHECK(compiled("'LAND'").empty());
}
