// A unit's Lua object is made by its script class, as Moho's
// CScriptObject::CreateLuaObject makes it (faf-re): calling the class runs
// its __init and __post_init. FAF's ACUs name their gun in __init; without
// it their ResetRightArm looks up a weapon labelled nil.

#include <catch2/catch_test_macros.hpp>

#include "blueprints/blueprint_store.hpp"
#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/sim_bindings.hpp"
#include "sim/script_class.hpp"
#include "sim/sim_state.hpp"

extern "C" {
#include <lua.h>
}

#include <string>

namespace {

// A class as class.lua makes one: calling it makes an instance and runs
// __init, then __post_init.
constexpr const char* kClassKit = R"(
    ClassFactory = {
        __call = function(cls)
            local o = {}
            setmetatable(o, cls)
            if cls.__init then cls.__init(o) end
            if cls.__post_init then cls.__post_init(o) end
            return o
        end
    }
    function MakeClass(body)
        body.__index = body
        return setmetatable(body, ClassFactory)
    end
)";

struct ScriptWorld {
    osc::lua::LuaState state;
    osc::blueprints::BlueprintStore store{state.raw()};
    osc::sim::SimState sim{state.raw(), &store};

    ScriptWorld() {
        osc::lua::register_moho_bindings(state, sim);
        osc::lua::register_sim_bindings(state, sim);
        sim.add_army("ARMY_1", "ARMY_1");
        lua_State* L = state.raw();
        for (const char* id : {"test_acu", "test_broken", "test_plain"}) {
            REQUIRE(state
                        .do_string(std::string("return {BlueprintId = '") + id +
                                   "', Defense = {MaxHealth = 100}}")
                        .ok());
            store.register_blueprint(L, osc::blueprints::BlueprintType::Unit, lua_gettop(L));
            lua_pop(L, 1);
        }
        store.expose_to_lua(L);
        REQUIRE(state.do_string(kClassKit).ok());
        REQUIRE(state
                    .do_string(R"(
            TestACU = MakeClass {
                __init = function(self)
                    self.rightGunLabel = 'RightZephyr'
                    self.inits = (self.inits or 0) + 1
                end,
                __post_init = function(self) self.post_inits = (self.post_inits or 0) + 1 end,
            }
            TestBroken = MakeClass { __init = function(self) error('no gun') end }
            TestPlain = { plain = true }
            TestPlain.__index = TestPlain
        )")
                    .ok());
        // Each blueprint's script class, as resolving its module would
        // cache it (there is no VFS here to import one from).
        lua_pushstring(L, "__osc_unit_script_classes");
        lua_newtable(L);
        for (const auto& [id, cls] :
             {std::pair{"test_acu", "TestACU"}, std::pair{"test_broken", "TestBroken"},
              std::pair{"test_plain", "TestPlain"}}) {
            lua_pushstring(L, id);
            lua_getglobal(L, cls);
            lua_rawset(L, -3);
        }
        lua_rawset(L, LUA_REGISTRYINDEX);
    }

    bool check(const char* code) {
        auto r = state.do_string(code);
        UNSCOPED_INFO((r.ok() ? std::string() : r.error().message));
        return r.ok();
    }
};

} // namespace

TEST_CASE("A unit's script class makes its object, running __init", "[scriptobject]") {
    ScriptWorld w;
    CHECK(w.check(R"(
        local u = CreateUnit('test_acu', 1, 0, 0, 0)
        assert(getmetatable(u) == TestACU, 'not an instance of its class')
        assert(u.rightGunLabel == 'RightZephyr', 'its __init did not run')
        assert(u.inits == 1 and u.post_inits == 1, 'init ' .. tostring(u.inits)
               .. ', post-init ' .. tostring(u.post_inits))
        assert(rawget(u, '_c_object'), 'no C++ unit behind it')
        assert(GetEntityById(u.EntityId) == u, 'not the object the sim hands out')
    )"));
}

TEST_CASE("A unit whose class fails to make it gets a plain instance", "[scriptobject]") {
    ScriptWorld w;
    CHECK(w.check(R"(
        local u = CreateUnit('test_broken', 1, 0, 0, 0)
        assert(getmetatable(u) == TestBroken, 'not an instance of its class')
        assert(rawget(u, '_c_object'), 'no C++ unit behind it')
        -- A class with no __call is its instance's metatable, as before.
        local p = CreateUnit('test_plain', 1, 0, 0, 0)
        assert(getmetatable(p) == TestPlain and p.plain, 'not an instance of its class')
    )"));
}

TEST_CASE("push_new_script_object makes an object as Moho does", "[scriptobject]") {
    osc::lua::LuaState state;
    lua_State* L = state.raw();
    REQUIRE(state.do_string(kClassKit).ok());
    REQUIRE(state
                .do_string(R"(
        Made = MakeClass { __init = function(self) self.made = true end }
        Odd = setmetatable({}, { __call = function() return 42 end })
        Uncallable = setmetatable({}, { __index = {} })
    )")
                .ok());
    const int top = lua_gettop(L);

    SECTION("a callable class runs its __init") {
        lua_getglobal(L, "Made");
        osc::sim::push_new_script_object(L, "Test");
        REQUIRE(lua_gettop(L) == top + 1);
        lua_pushstring(L, "made");
        lua_rawget(L, -2);
        CHECK(lua_toboolean(L, -1));
    }
    SECTION("a call that returns no table gives a plain instance") {
        lua_getglobal(L, "Odd");
        osc::sim::push_new_script_object(L, "Test");
        REQUIRE(lua_gettop(L) == top + 1);
        REQUIRE(lua_istable(L, -1));
        REQUIRE(lua_getmetatable(L, -1));
        lua_getglobal(L, "Odd");
        CHECK(lua_rawequal(L, -1, -2));
    }
    SECTION("a class whose metatable has no __call is its instance's metatable") {
        lua_getglobal(L, "Uncallable");
        osc::sim::push_new_script_object(L, "Test");
        REQUIRE(lua_gettop(L) == top + 1);
        REQUIRE(lua_getmetatable(L, -1));
        lua_getglobal(L, "Uncallable");
        CHECK(lua_rawequal(L, -1, -2));
    }
    SECTION("no class gives an empty table") {
        lua_pushnil(L);
        osc::sim::push_new_script_object(L, "Test");
        REQUIRE(lua_gettop(L) == top + 1);
        CHECK(lua_istable(L, -1));
        CHECK_FALSE(lua_getmetatable(L, -1));
    }
}
