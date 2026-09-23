#include <catch2/catch_test_macros.hpp>

#include "sim/blueprint_categories.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <string>
#include <unordered_set>

namespace {

struct LuaGuard {
    lua_State* L = lua_open();
    ~LuaGuard() { lua_close(L); }
};

std::unordered_set<std::string> categories_of(const char* bp_source) {
    LuaGuard g;
    const std::string chunk = std::string("return ") + bp_source;
    REQUIRE(luaL_loadbuffer(g.L, chunk.data(), chunk.size(), "bp") == 0);
    REQUIRE(lua_pcall(g.L, 0, 1, 0) == 0);
    const int top = lua_gettop(g.L);
    std::unordered_set<std::string> out;
    osc::sim::collect_blueprint_categories(g.L, top, out);
    CHECK(lua_gettop(g.L) == top); // stack balanced
    return out;
}

} // namespace

TEST_CASE("retail blueprints: categories come from the Categories list",
          "[categories]") {
    // Retail Blueprints.lua never builds CategoriesHash; the engine reads the
    // plain list itself.
    auto cats = categories_of("{ Categories = { 'COMMAND', 'LAND', 'UEF' } }");
    CHECK(cats == std::unordered_set<std::string>{"COMMAND", "LAND", "UEF"});
}

TEST_CASE("FAF blueprints: CategoriesHash keys are used", "[categories]") {
    auto cats = categories_of("{ CategoriesHash = { COMMAND = true, LAND = true } }");
    CHECK(cats == std::unordered_set<std::string>{"COMMAND", "LAND"});
}

TEST_CASE("both forms present: union, so derived hash entries survive",
          "[categories]") {
    auto cats = categories_of(
        "{ Categories = { 'COMMAND' }, CategoriesHash = { COMMAND = true, TECH1 = true } }");
    CHECK(cats == std::unordered_set<std::string>{"COMMAND", "TECH1"});
}

TEST_CASE("missing or malformed category data yields nothing", "[categories]") {
    CHECK(categories_of("{}").empty());
    CHECK(categories_of("{ Categories = 'COMMAND' }").empty());
    CHECK(categories_of("{ Categories = { 1, true, 'LAND' } }") ==
          std::unordered_set<std::string>{"LAND"});
}
