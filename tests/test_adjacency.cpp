#include <catch2/catch_test_macros.hpp>

#include "sim/entity_registry.hpp"
#include "sim/manipulator.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include <memory>
#include <string>
#include <vector>

using osc::f32;
using osc::sim::CollisionShapeType;
using osc::sim::SimState;
using osc::sim::Unit;

namespace {

struct Base {
    lua_State* L = lua_open();
    SimState sim{L, nullptr};

    Base() {
        luaopen_base(L);
        luaopen_table(L);
        lua_settop(L, 0);
        const char* code = R"(
            __pairs = {}
            Structure = {
                OnAdjacentTo = function(self, other, trigger)
                    table.insert(__pairs, self.Name .. ' ' .. other.Name)
                end,
                OnNotAdjacentTo = function(self, other)
                    table.insert(__pairs, 'not ' .. self.Name .. ' ' .. other.Name)
                end,
            }
            Structure.__index = Structure
        )";
        REQUIRE(luaL_loadbuffer(L, code, std::string(code).size(), "stub") == 0);
        REQUIRE(lua_pcall(L, 0, 0, 0) == 0);
        sim.entity_registry().init_spatial_grid(256, 256);
    }
    ~Base() { lua_close(L); }
    Base(const Base&) = delete;
    Base& operator=(const Base&) = delete;

    Unit* make(const char* name, f32 x, f32 z, f32 footprint, f32 skirt_offset, f32 skirt,
               bool being_built = false) {
        auto u = std::make_unique<Unit>();
        u->set_army(0);
        u->add_category("STRUCTURE");
        u->set_position({x, 0.0f, z});
        u->set_footprint_size(footprint, footprint);
        u->set_skirt(skirt, skirt, skirt_offset, skirt_offset);
        u->set_collision_shape(
            {CollisionShapeType::BOX, 0.0f, 0.5f, 0.0f, footprint * 0.5f, 0.5f, footprint * 0.5f});
        Unit* raw = u.get();
        sim.entity_registry().register_entity(std::move(u));
        lua_newtable(L);
        lua_pushstring(L, "Name");
        lua_pushstring(L, name);
        lua_settable(L, -3);
        lua_pushstring(L, "Structure");
        lua_gettable(L, LUA_GLOBALSINDEX);
        lua_setmetatable(L, -2);
        raw->set_lua_table_ref(luaL_ref(L, LUA_REGISTRYINDEX));
        raw->set_is_being_built(being_built);
        if (!being_built) {
            raw->fire_adjacency_callbacks(sim, L);
        }
        return raw;
    }
    Unit* generator(const char* name, f32 x, f32 z, bool being_built = false) {
        return make(name, x, z, 1.0f, -0.5f, 2.0f, being_built);
    }
    Unit* factory(const char* name, f32 x, f32 z) { return make(name, x, z, 5.0f, -1.5f, 8.0f); }

    std::vector<std::string> pairs() {
        std::vector<std::string> out;
        lua_pushstring(L, "__pairs");
        lua_gettable(L, LUA_GLOBALSINDEX);
        for (int i = 1;; ++i) {
            lua_rawgeti(L, -1, i);
            if (lua_isnil(L, -1)) {
                lua_pop(L, 1);
                break;
            }
            out.emplace_back(lua_tostring(L, -1));
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
        return out;
    }
};

} // namespace

TEST_CASE("Structures are adjacent where one's side spans the other's along the edge they share",
          "[adjacency]") {
    SECTION("two generators side by side") {
        Base b;
        b.generator("a", 40.5f, 40.5f);
        b.generator("b", 42.5f, 40.5f);
        CHECK(b.pairs() == std::vector<std::string>{"b a", "a b"});
    }
    SECTION("two generators a cell apart along that edge") {
        Base b;
        b.generator("a", 40.5f, 40.5f);
        b.generator("b", 42.5f, 41.5f);
        CHECK(b.pairs().empty());
    }
    SECTION("a generator within a factory's side") {
        Base b;
        b.factory("f", 70.5f, 70.5f);
        b.generator("c", 75.5f, 70.5f);
        CHECK(b.pairs() == std::vector<std::string>{"c f", "f c"});
    }
    SECTION("a generator past a factory's corner") {
        Base b;
        b.factory("f", 70.5f, 70.5f);
        b.generator("d", 75.5f, 74.5f);
        CHECK(b.pairs().empty());
    }
}

TEST_CASE("A structure's skirt starts from its footprint's corner rounded to a whole cell",
          "[adjacency]") {
    for (const f32 off : {0.0f, 0.5f}) {
        CAPTURE(off);
        Base b;
        b.factory("f", 100.0f + off, 100.0f + off);
        b.generator("c", 105.0f + off, 100.0f + off);
        CHECK(b.pairs().empty() == (off == 0.0f));
    }
}

TEST_CASE("A structure hears of its neighbours in the order they were made", "[adjacency]") {
    Base b;
    b.generator("e", 62.5f, 60.5f);
    b.generator("w", 58.5f, 60.5f);
    b.generator("n", 60.5f, 62.5f);
    b.generator("s", 60.5f, 58.5f);
    b.generator("q", 60.5f, 60.5f);
    CHECK(b.pairs() ==
          std::vector<std::string>{"q e", "e q", "q w", "w q", "q n", "n q", "q s", "s q"});
}

TEST_CASE("A structure that dies tells the structures it overlaps then", "[adjacency]") {
    SECTION("a neighbour started after it was finished, still being built") {
        Base b;
        Unit* a = b.generator("a", 40.5f, 40.5f);
        b.generator("b", 42.5f, 40.5f, true);
        b.sim.drop_adjacency(*a);
        CHECK(b.pairs() == std::vector<std::string>{"not a b", "not b a"});
    }
    SECTION("none, while it is being built") {
        Base b;
        Unit* a = b.generator("a", 40.5f, 40.5f, true);
        b.generator("b", 42.5f, 40.5f);
        b.sim.drop_adjacency(*a);
        CHECK(b.pairs() == std::vector<std::string>{"b a", "a b"});
    }
}
