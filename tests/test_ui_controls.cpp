#include <catch2/catch_test_macros.hpp>

#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "sim/sim_state.hpp"
#include "ui/ui_control.hpp"
#include "ui/ui_dispatch.hpp"

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

TEST_CASE("Destroying the root frame clears it but keeps it", "[ui][lua]") {
    // Retail's Load and replay dialogs, opened in a game, destroy the
    // control they were opened over -- GetFrame(0) -- as they leave for the
    // next game. The next game builds its interface on the same frame.
    osc::lua::LuaState lua;
    osc::sim::SimState sim(lua.raw(), nullptr);
    osc::ui::UIControlRegistry registry;
    osc::lua::register_moho_bindings(lua, sim);
    osc::lua::register_ui_bindings(lua, registry);

    auto result = lua.do_string(R"(
        local function make(parent)
            local c = {}
            setmetatable(c, { __index = moho.control_methods })
            InternalCreateGroup(c, parent)
            return c
        end
        local panel = make(GetFrame(0))
        make(panel)
        panel_gone = false
        panel.OnDestroy = function(self) panel_gone = true end
        -- A frame's Destroy is Control's (retail's Frame class derives it).
        moho.control_methods.Destroy(GetFrame(0))
        if not panel_gone then error('what the frame held survived') end
        local after = make(GetFrame(0))
        if moho.control_methods.GetParent(after) ~= GetFrame(0) then
            error('the frame no longer takes children')
        end
    )");
    INFO((result.ok() ? std::string() : result.error().message));
    REQUIRE(result.ok());
    lua_State* L = lua.raw();
    lua_pushstring(L, "__osc_root_frame");
    lua_rawget(L, LUA_REGISTRYINDEX);
    lua_pushstring(L, "_c_object");
    lua_rawget(L, -2);
    auto* root = static_cast<osc::ui::UIControl*>(lua_touserdata(L, -1));
    lua_pop(L, 2);
    REQUIRE(root != nullptr);
    CHECK_FALSE(root->destroyed());
    CHECK(root->children().size() == 1);
}

namespace {
osc::ui::UIControl* control_of(lua_State* L, const char* global) {
    lua_getglobal(L, global);
    lua_pushstring(L, "_c_object");
    lua_rawget(L, -2);
    auto* c = static_cast<osc::ui::UIControl*>(lua_touserdata(L, -1));
    lua_pop(L, 2);
    return c;
}
} // namespace

TEST_CASE("UI hit-testing picks the deepest control as Moho does", "[ui][lua]") {
    osc::lua::LuaState lua;
    osc::sim::SimState sim(lua.raw(), nullptr);
    osc::ui::UIControlRegistry registry;
    osc::lua::register_moho_bindings(lua, sim);
    osc::lua::register_ui_bindings(lua, registry);

    // Plain numbers stand in for the layout LazyVars (no LazyVar module here).
    auto result = lua.do_string(R"(
        local function box(parent, l, t, r, b, depth)
            local c = {}
            setmetatable(c, { __index = moho.control_methods })
            InternalCreateGroup(c, parent)
            rawset(c, 'Left', l) rawset(c, 'Top', t)
            rawset(c, 'Right', r) rawset(c, 'Bottom', b)
            rawset(c, 'Width', r - l) rawset(c, 'Height', b - t)
            rawset(c, 'Depth', depth)
            return c
        end
        back = box(GetFrame(0), 0, 0, 100, 100, 10)     -- created first, above
        front = box(GetFrame(0), 50, 50, 150, 150, 5)   -- created later, below
        container = box(GetFrame(0), 200, 0, 400, 100, 20)
        container:DisableHitTest()
        child = box(container, 250, 20, 300, 60, 21)
    )");
    INFO((result.ok() ? std::string() : result.error().message));
    REQUIRE(result.ok());

    lua_State* L = lua.raw();
    lua_pushstring(L, "__osc_root_frame");
    lua_rawget(L, LUA_REGISTRYINDEX);
    lua_pushstring(L, "_c_object");
    lua_rawget(L, -2);
    auto* root = static_cast<osc::ui::UIControl*>(lua_touserdata(L, -1));
    lua_pop(L, 2);
    REQUIRE(root != nullptr);

    osc::ui::UIDispatch dispatch;
    // Overlap: depth decides, not creation order.
    CHECK(dispatch.hit_test(L, root, 75, 75) == control_of(L, "back"));
    CHECK(dispatch.hit_test(L, root, 120, 120) == control_of(L, "front"));
    // A container with hit-testing disabled passes clicks to its children.
    CHECK(dispatch.hit_test(L, root, 260, 30) == control_of(L, "child"));
    CHECK(dispatch.hit_test(L, root, 350, 80) == nullptr);
}
