#include <catch2/catch_test_macros.hpp>

#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "sim/sim_state.hpp"
#include "ui/ui_control.hpp"
#include "ui/console.hpp"
#include "ui/keymap.hpp"
#include "ui/ui_dispatch.hpp"
#include "ui/world_view.hpp"

#include <GLFW/glfw3.h>

#include <memory>
#include <string>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
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

namespace {

/// A UI state with the input fixtures: boxes laid out in plain numbers, each
/// logging the events it handles (and consuming them if `eats` is set).
struct InputFixture {
    osc::lua::LuaState lua;
    osc::sim::SimState sim{lua.raw(), nullptr};
    osc::ui::UIControlRegistry registry;
    osc::ui::KeyMapRegistry keymap;
    osc::ui::Console console;
    osc::ui::UIDispatch dispatch;

    InputFixture() {
        osc::lua::register_moho_bindings(lua, sim);
        osc::lua::register_ui_bindings(lua, registry);
        lua_State* L = lua.raw();
        lua_pushstring(L, "__osc_keymap_registry");
        lua_pushlightuserdata(L, &keymap);
        lua_rawset(L, LUA_REGISTRYINDEX);
        lua_pushstring(L, "__osc_console");
        lua_pushlightuserdata(L, &console);
        lua_rawset(L, LUA_REGISTRYINDEX);
        osc::lua::register_console_commands(console);
        // Q's name, as keyNames.lua gives it (by virtual-key code).
        REQUIRE(lua.do_string("__names = { ['51'] = 'Q' }").ok());
        lua_getglobal(L, "__names");
        keymap.set_key_names(L, -1);
        lua_pop(L, 1);
        auto result = lua.do_string(R"(
            handled = {}
            hotkeys = 0
            IN_AddKeyMapTable({ Q = { action = 'UI_Lua hotkeys = hotkeys + 1' } })
            function box(name, parent, l, t, r, b, depth)
                local c = {}
                setmetatable(c, { __index = moho.control_methods })
                InternalCreateGroup(c, parent)
                rawset(c, 'Left', l) rawset(c, 'Top', t)
                rawset(c, 'Right', r) rawset(c, 'Bottom', b)
                rawset(c, 'Width', r - l) rawset(c, 'Height', b - t)
                rawset(c, 'Depth', depth)
                c.HandleEvent = function(self, event)
                    table.insert(handled, { who = name, event = event })
                    return self.eats
                end
                return c
            end
            -- The events of a type handled, in order: {who, event} each.
            function of_type(type)
                local out = {}
                for _, e in handled do
                    if e.event.Type == type then table.insert(out, e) end
                end
                return out
            end
            -- Who handled them, as 'a,b,c'.
            function whos(type)
                local names = {}
                for _, e in of_type(type) do table.insert(names, e.who) end
                return table.concat(names, ',')
            end
        )");
        INFO((result.ok() ? std::string() : result.error().message));
        REQUIRE(result.ok());
    }
    InputFixture(const InputFixture&) = delete;
    InputFixture& operator=(const InputFixture&) = delete;

    void run(const char* code) {
        auto result = lua.do_string(code);
        INFO(code);
        INFO((result.ok() ? std::string() : result.error().message));
        REQUIRE(result.ok());
    }
    bool check(const char* expr) {
        auto result = lua.do_string(std::string("__check = ") + expr);
        INFO(expr);
        INFO((result.ok() ? std::string() : result.error().message));
        REQUIRE(result.ok());
        lua_State* L = lua.raw();
        lua_getglobal(L, "__check");
        const bool ok = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        return ok;
    }
    void deliver() { dispatch.dispatch_events(lua.raw(), registry); }
};

} // namespace

TEST_CASE("UI events reach Lua with Moho's codes and modifiers", "[ui][lua][input]") {
    InputFixture f;
    f.run("target = box('target', GetFrame(0), 0, 0, 100, 100, 1) target.eats = true");
    f.registry.set_keyboard_focus(control_of(f.lua.raw(), "target"));

    f.dispatch.on_key(GLFW_KEY_ESCAPE, GLFW_PRESS, 0);
    f.dispatch.on_key(GLFW_KEY_F1, GLFW_RELEASE, GLFW_MOD_SHIFT);
    f.deliver();
    CHECK(f.check("of_type('KeyDown')[1].event.KeyCode == 27 "
                  "and of_type('KeyDown')[1].event.RawKeyCode == 27"));
    CHECK(f.check("of_type('KeyUp')[1].event.KeyCode == 342 "
                  "and of_type('KeyUp')[1].event.RawKeyCode == 112 "
                  "and of_type('KeyUp')[1].event.Modifiers.Shift"));

    // The mouse: wx's button numbers, and the buttons held after the event.
    // A key event carries no buttons, even with one held.
    f.run("handled = {}");
    f.dispatch.on_mouse_button(GLFW_MOUSE_BUTTON_LEFT, GLFW_PRESS, 0); // at (0, 0)
    f.dispatch.on_key(GLFW_KEY_A, GLFW_PRESS, 0);
    f.dispatch.on_cursor_pos(50, 50);
    f.deliver();
    f.registry.set_keyboard_focus(nullptr);
    CHECK(
        f.check("whos('ButtonPress') == 'target' and of_type('ButtonPress')[1].event.KeyCode == 1 "
                "and of_type('ButtonPress')[1].event.Modifiers.Left "
                "and not of_type('ButtonPress')[1].event.Modifiers.Right"));
    CHECK(f.check("of_type('KeyDown')[1].event.KeyCode == 65 "
                  "and of_type('KeyDown')[1].event.Modifiers.Left == nil"));
    CHECK(f.check("of_type('MouseMotion')[1].event.KeyCode == 0 "
                  "and of_type('MouseMotion')[1].event.RawKeyCode == 0 "
                  "and of_type('MouseMotion')[1].event.Modifiers.Left"));

    f.run("handled = {}");
    f.dispatch.on_mouse_button(GLFW_MOUSE_BUTTON_RIGHT, GLFW_PRESS, GLFW_MOD_CONTROL);
    f.dispatch.on_mouse_button(GLFW_MOUSE_BUTTON_LEFT, GLFW_RELEASE, 0);
    f.dispatch.on_mouse_button(GLFW_MOUSE_BUTTON_MIDDLE, GLFW_PRESS, 0);
    f.deliver();
    CHECK(f.check("of_type('ButtonPress')[1].event.KeyCode == 3 "
                  "and of_type('ButtonPress')[1].event.Modifiers.Right "
                  "and of_type('ButtonPress')[1].event.Modifiers.Left "
                  "and of_type('ButtonPress')[1].event.Modifiers.Ctrl"));
    CHECK(f.check("of_type('ButtonRelease')[1].event.KeyCode == 1 "
                  "and not of_type('ButtonRelease')[1].event.Modifiers.Left "
                  "and of_type('ButtonRelease')[1].event.Modifiers.Right"));
    CHECK(f.check("of_type('ButtonPress')[2].event.KeyCode == 2 "
                  "and of_type('ButtonPress')[2].event.Modifiers.Middle"));
}

TEST_CASE("An input capture takes the mouse and the keys, as Moho's", "[ui][lua][input]") {
    InputFixture f;
    f.run(R"(
        other = box('other', GetFrame(0), 0, 0, 100, 100, 1)
        other.eats = true
        screen = box('screen', GetFrame(0), 200, 0, 400, 100, 1)
        inner = box('inner', screen, 250, 20, 300, 60, 2)
    )");
    CHECK(f.check("AnyInputCapture() == false and GetInputCapture() == nil"));
    f.run("AddInputCapture(screen)");
    CHECK(f.check("AnyInputCapture() and GetInputCapture() == screen"));

    // A click over another control goes to the capture instead.
    f.dispatch.on_cursor_pos(50, 50);
    f.dispatch.on_mouse_button(GLFW_MOUSE_BUTTON_LEFT, GLFW_PRESS, 0);
    f.deliver();
    CHECK(f.check("whos('ButtonPress') == 'screen'"));
    CHECK(f.check("not string.find(whos('ButtonPress') .. whos('MouseMotion'), 'other')"));
    // One over the capture's own children reaches them first, then it.
    f.run("handled = {}");
    f.dispatch.on_cursor_pos(260, 30);
    f.dispatch.on_mouse_button(GLFW_MOUSE_BUTTON_LEFT, GLFW_RELEASE, 0);
    f.deliver();
    CHECK(f.check("string.find(whos('ButtonRelease'), '^inner,screen')"));

    // With no focus the capture takes the keys, and the key map never sees them.
    f.run("handled = {}");
    f.dispatch.on_key(GLFW_KEY_Q, GLFW_PRESS, 0);
    f.deliver();
    CHECK(f.check("whos('KeyDown') == 'screen' and of_type('KeyDown')[1].event.KeyCode == 81 "
                  "and hotkeys == 0"));
    // The focus comes first.
    f.registry.set_keyboard_focus(control_of(f.lua.raw(), "other"));
    f.run("handled = {}");
    f.dispatch.on_key(GLFW_KEY_Q, GLFW_PRESS, 0);
    f.deliver();
    CHECK(f.check("whos('KeyDown') == 'other' and hotkeys == 0"));
    // Even a key it ignores goes no further: the key map never sees it.
    f.run("other.eats = false handled = {}");
    f.dispatch.on_key(GLFW_KEY_Q, GLFW_PRESS, 0);
    f.deliver();
    CHECK(f.check("whos('KeyDown') == 'other' and hotkeys == 0"));
    f.run("other.eats = true");
    f.registry.set_keyboard_focus(nullptr);

    // Captures stack; removal takes the last entry of that control.
    f.run("AddInputCapture(other) AddInputCapture(screen) AddInputCapture(other)");
    CHECK(f.check("GetInputCapture() == other"));
    f.run("RemoveInputCapture(other)");
    CHECK(f.check("GetInputCapture() == screen"));
    f.run("RemoveInputCapture(screen)");
    CHECK(f.check("GetInputCapture() == other"));
    f.run("RemoveInputCapture(other)");
    CHECK(f.check("GetInputCapture() == screen"));

    // A destroyed control leaves the stack.
    f.run("screen:Destroy()");
    CHECK(f.check("AnyInputCapture() == false and GetInputCapture() == nil"));
    // Without a capture, keys go on to the key map, clicks to what they hit.
    f.run("handled = {}");
    f.dispatch.on_key(GLFW_KEY_Q, GLFW_PRESS, 0);
    f.dispatch.on_cursor_pos(50, 50);
    f.dispatch.on_mouse_button(GLFW_MOUSE_BUTTON_LEFT, GLFW_PRESS, 0);
    f.deliver();
    CHECK(f.check("hotkeys == 1 and whos('ButtonPress') == 'other'"));

    // A held key's auto-repeat acts only for a binding that asks (keyRepeat).
    f.dispatch.on_key(GLFW_KEY_Q, GLFW_REPEAT, 0);
    f.deliver();
    CHECK(f.check("hotkeys == 1"));
    f.run("IN_AddKeyMapTable({ Q = { action = 'UI_Lua hotkeys = hotkeys + 10', keyRepeat = true } "
          "})");
    f.dispatch.on_key(GLFW_KEY_Q, GLFW_REPEAT, 0);
    f.deliver();
    CHECK(f.check("hotkeys == 11"));
}

TEST_CASE("The world has the mouse only where no UI, and no capture, holds it",
          "[ui][lua][input]") {
    InputFixture f;
    lua_State* L = f.lua.raw();
    // A world view over the screen, made as UIWorldView.__init makes one.
    {
        auto view = std::make_unique<osc::ui::WorldView>();
        osc::ui::WorldView* wv = view.get();
        f.registry.add(std::move(view));
        f.run("world = {} __root_frame_table = GetFrame(0)");
        lua_getglobal(L, "world");
        lua_pushstring(L, "_c_object");
        lua_pushlightuserdata(L, wv);
        lua_rawset(L, -3);
        wv->set_lua_table_ref(luaL_ref(L, LUA_REGISTRYINDEX));
        wv->set_parent(control_of(L, "__root_frame_table"));
    }
    f.run(R"(
        rawset(world, 'Left', 0) rawset(world, 'Top', 0)
        rawset(world, 'Right', 400) rawset(world, 'Bottom', 300)
        rawset(world, 'Width', 400) rawset(world, 'Height', 300)
        rawset(world, 'Depth', 1)
        panel = box('panel', GetFrame(0), 0, 0, 100, 100, 5)
    )");
    const auto ui_has = [&](double x, double y) {
        return f.dispatch.ui_has_mouse(L, f.registry, x, y);
    };
    CHECK(ui_has(50, 50));         // the panel
    CHECK_FALSE(ui_has(200, 200)); // the world view
    // A capture takes the mouse from the world...
    f.run("AddInputCapture(panel)");
    CHECK(ui_has(200, 200));
    // ...unless the world view is under it in the capture.
    f.run("RemoveInputCapture(panel) AddInputCapture(GetFrame(0))");
    CHECK_FALSE(ui_has(200, 200));
    CHECK(ui_has(50, 50));
    f.run("RemoveInputCapture(GetFrame(0))");
    CHECK_FALSE(ui_has(200, 200));
}
