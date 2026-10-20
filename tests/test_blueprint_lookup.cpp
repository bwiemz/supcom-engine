#include <catch2/catch_test_macros.hpp>

#include "blueprints/blueprint_store.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include <string>

using osc::blueprints::BlueprintStore;
using osc::blueprints::BlueprintType;
using osc::blueprints::push_blueprint;
using osc::blueprints::push_blueprints;

namespace {

struct Blueprints {
    lua_State* L = lua_open();
    BlueprintStore store{L};

    Blueprints() {
        luaopen_base(L);
        lua_settop(L, 0);
        run("return { BlueprintId = 'XEA3204', Economy = { BuildCostEnergy = 1000 } }");
        store.register_blueprint(L, BlueprintType::Unit, lua_gettop(L));
        lua_pop(L, 1);
        store.expose_to_lua(L);
    }
    ~Blueprints() {
        store.rebind(nullptr);
        lua_close(L);
    }

    void run(const char* chunk) const {
        REQUIRE(luaL_loadbuffer(L, chunk, std::char_traits<char>::length(chunk), "test") == 0);
        REQUIRE(lua_pcall(L, 0, LUA_MULTRET, 0) == 0);
    }

    /// `id`'s Economy.BuildCostEnergy, or -1 without its table; checks the
    /// stack has one value more on a find and none on a miss.
    double build_cost_energy(const char* id) const {
        const int top = lua_gettop(L);
        if (!push_blueprint(L, id)) {
            CHECK(lua_gettop(L) == top);
            return -1;
        }
        CHECK(lua_gettop(L) == top + 1);
        lua_pushstring(L, "Economy");
        lua_rawget(L, -2);
        lua_pushstring(L, "BuildCostEnergy");
        lua_rawget(L, -2);
        const double value = lua_tonumber(L, -1);
        lua_settop(L, top);
        return value;
    }
};

} // namespace

TEST_CASE("push_blueprint finds an id in any case", "[blueprints]") {
    Blueprints b;
    // Retail's MaintainPodsState asks for its pod as 'XEA3204'.
    CHECK(b.build_cost_energy("XEA3204") == 1000);
    CHECK(b.build_cost_energy("xea3204") == 1000);
    CHECK(b.build_cost_energy("XeA3204") == 1000);
}

TEST_CASE("push_blueprint pushes nothing for a missing id", "[blueprints]") {
    Blueprints b;
    CHECK(b.build_cost_energy("xea9999") == -1);
    CHECK(b.build_cost_energy("") == -1);
    // A binding answers a miss with nil, as Moho's do.
    const int top = lua_gettop(b.L);
    if (!push_blueprint(b.L, "xea9999")) lua_pushnil(b.L);
    CHECK(lua_gettop(b.L) == top + 1);
    CHECK(lua_isnil(b.L, -1));
}

TEST_CASE("push_blueprint pushes nothing before the blueprints are exposed", "[blueprints]") {
    lua_State* L = lua_open();
    CHECK_FALSE(push_blueprint(L, "xea3204"));
    CHECK_FALSE(push_blueprints(L));
    CHECK(lua_gettop(L) == 0);
    lua_close(L);
}

TEST_CASE("push_blueprint asks the table's __index with the id as given", "[blueprints]") {
    Blueprints b;
    // A mod's alias: an old id that names another blueprint.
    b.run(R"(
        setmetatable(__blueprints, { __index = function(t, id)
            if id == 'OldPod' then return rawget(t, 'xea3204') end
        end })
    )");
    CHECK(b.build_cost_energy("OldPod") == 1000);
    CHECK(b.build_cost_energy("oldpod") == -1);
    CHECK(b.build_cost_energy("XEA3204") == 1000);
    CHECK(b.build_cost_energy("xea9999") == -1);
}

TEST_CASE("push_blueprints pushes the table keyed by lowercase id", "[blueprints]") {
    Blueprints b;
    REQUIRE(push_blueprints(b.L));
    lua_pushstring(b.L, "xea3204");
    lua_rawget(b.L, -2);
    CHECK(lua_istable(b.L, -1));
    lua_settop(b.L, 0);
}
