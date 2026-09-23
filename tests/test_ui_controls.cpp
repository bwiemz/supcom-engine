#include <catch2/catch_test_macros.hpp>

#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "sim/sim_state.hpp"
#include "ui/ui_control.hpp"

extern "C" {
#include <lua.h>
}

TEST_CASE("Control:Destroy is safe from its own OnDestroy", "[ui][lua]") {
    // Moho destroys a control's children with it, calling each OnDestroy.
    // Scripts destroy things from OnDestroy -- themselves, their parent --
    // which must not re-run a teardown already in progress (twice-run
    // OnDestroy, a registry ref freed twice, or endless recursion).
    osc::lua::LuaState lua;
    osc::sim::SimState sim(lua.raw(), nullptr);
    osc::ui::UIControlRegistry registry;
    osc::lua::register_moho_bindings(lua, sim);
    osc::lua::register_ui_bindings(lua, registry);

    auto result = lua.do_string(R"(
        destroyed = { a = 0, b = 0, c = 0 }
        local function make(parent, name)
            local c = {}
            setmetatable(c, { __index = moho.control_methods })
            InternalCreateGroup(c, parent)
            c.OnDestroy = function(self) destroyed[name] = destroyed[name] + 1 end
            return c
        end
        local a = make(GetFrame(0), 'a')
        local b = make(a, 'b')
        make(b, 'c')
        b.OnDestroy = function(self)
            destroyed.b = destroyed.b + 1
            a:Destroy()       -- the parent, mid-teardown
            self:Destroy()    -- itself, mid-teardown
        end
        a:Destroy()
        a:Destroy()           -- already gone: a no-op
    )");
    INFO((result.ok() ? std::string() : result.error().message));
    REQUIRE(result.ok());

    lua_State* L = lua.raw();
    lua_getglobal(L, "destroyed");
    for (const char* name : {"a", "b", "c"}) {
        INFO(name);
        lua_pushstring(L, name);
        lua_rawget(L, -2);
        CHECK(lua_tonumber(L, -1) == 1.0);
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
}
