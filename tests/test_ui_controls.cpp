#include <catch2/catch_test_macros.hpp>

#include "core/test_status.hpp"
#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "sim/sim_state.hpp"
#include "ui/ui_control.hpp"
#include "ui/console.hpp"
#include "ui/keymap.hpp"
#include "ui/scroll.hpp"
#include "ui/edit_text.hpp"
#include "ui/ui_dispatch.hpp"
#include "ui/world_view.hpp"

#include <GLFW/glfw3.h>

#include <memory>
#include <string>
#include <vector>

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

TEST_CASE("Each control constructor runs the control's OnInit, as Moho's do", "[ui][lua]") {
    osc::lua::LuaState lua;
    osc::sim::SimState sim(lua.raw(), nullptr);
    osc::ui::UIControlRegistry registry;
    osc::lua::register_moho_bindings(lua, sim);
    osc::lua::register_ui_bindings(lua, registry);

    const char* kControls[] = {"Group", "Frame",      "Bitmap",    "Text",
                               "Edit",  "ItemList",   "Scrollbar", "Border",
                               "Movie", "MapPreview", "Histogram"};
    for (const char* name : kControls) {
        INFO(name);
        auto result = lua.do_string(std::string(R"(
            inited = false
            local c = setmetatable({}, { __index = { OnInit = function(self) inited = true end } })
            InternalCreate)") + name +
                                    "(c, GetFrame(0))");
        INFO((result.ok() ? std::string() : result.error().message));
        REQUIRE(result.ok());
        lua_State* L = lua.raw();
        lua_getglobal(L, "inited");
        CHECK(lua_toboolean(L, -1));
        lua_pop(L, 1);
    }
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
        first = box(GetFrame(0), 500, 0, 600, 100, 7)   -- a tie: the first made wins
        later = box(GetFrame(0), 550, 50, 650, 150, 7)
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
    // Equal depths: the first in the walk, the first made (Moho's
    // GetTopmostControl keeps a control only for a deeper one). Retail's
    // score screen has its page group over its Continue button so.
    CHECK(dispatch.hit_test(L, root, 575, 75) == control_of(L, "first"));
    CHECK(dispatch.hit_test(L, root, 625, 125) == control_of(L, "later"));
}

TEST_CASE("Hiding a control hides its children, each told by OnHide, as Moho's", "[ui][lua]") {
    osc::lua::LuaState lua;
    osc::sim::SimState sim(lua.raw(), nullptr);
    osc::ui::UIControlRegistry registry;
    osc::lua::register_moho_bindings(lua, sim);
    osc::lua::register_ui_bindings(lua, registry);
    auto result = lua.do_string(R"(
        local function group(parent)
            local c = {}
            setmetatable(c, { __index = moho.control_methods })
            InternalCreateGroup(c, parent)
            return c
        end
        heard = {}
        parent = group(GetFrame(0))
        child = group(parent)
        child.OnHide = function(self, hidden) table.insert(heard, hidden) end
        -- A Window's border sits beside it, hidden by its OnHide
        beside = group(GetFrame(0))
        parent.OnHide = function(self, hidden) beside:SetHidden(hidden) end
        -- An OnHide returning true keeps its children as they are
        keeper = group(GetFrame(0))
        kept = group(keeper)
        keeper.OnHide = function() return true end

        parent:Hide()
        hidden_then = {parent:IsHidden(), child:IsHidden(), beside:IsHidden()}
        parent:Show()
        shown_then = {parent:IsHidden(), child:IsHidden(), beside:IsHidden()}
        keeper:Hide()
        kept_then = {keeper:IsHidden(), kept:IsHidden()}
        movie = {}
        setmetatable(movie, { __index = moho.control_methods })
        InternalCreateMovie(movie, GetFrame(0))
    )");
    INFO((result.ok() ? std::string() : result.error().message));
    REQUIRE(result.ok());
    auto check = lua.do_string(R"(
        local function show(t)
            return tostring(t[1]) .. ',' .. tostring(t[2]) .. ',' .. tostring(t[3])
        end
        assert(hidden_then[1] and hidden_then[2] and hidden_then[3], 'Hide: ' .. show(hidden_then))
        assert(not shown_then[1] and not shown_then[2] and not shown_then[3],
               'Show: ' .. show(shown_then))
        assert(heard[1] == true and heard[2] == false, 'OnHide heard ' .. show(heard))
        assert(kept_then[1] and not kept_then[2], 'kept: ' .. show(kept_then))
        -- A movie takes clicks unless a script says not (a timeline's skip)
        assert(not movie:IsHitTestDisabled(), 'a movie is hit-tested')
    )");
    INFO((check.ok() ? std::string() : check.error().message));
    CHECK(check.ok());
}

TEST_CASE("A frame's topmost depth is its deepest live control's, as Moho's", "[ui][lua]") {
    osc::lua::LuaState lua;
    osc::sim::SimState sim(lua.raw(), nullptr);
    osc::ui::UIControlRegistry registry;
    osc::lua::register_moho_bindings(lua, sim);
    osc::lua::register_ui_bindings(lua, registry);

    // As retail answers: a hidden control counts, and so do its children
    auto result = lua.do_string(R"(
        local function group(parent, depth)
            local c = setmetatable({}, { __index = moho.control_methods })
            InternalCreateGroup(c, parent)
            rawset(c, 'Depth', depth)
            return c
        end
        local frame = GetFrame(0)
        local g = group(frame, 5000)
        with_group = frame:GetTopmostDepth()
        g:Hide()
        hidden = frame:GetTopmostDepth()
        group(g, 7000)
        with_child = frame:GetTopmostDepth()
        g:Destroy()
        destroyed = frame:GetTopmostDepth()
    )");
    INFO((result.ok() ? std::string() : result.error().message));
    REQUIRE(result.ok());

    lua_State* L = lua.raw();
    auto global = [L](const char* name) {
        lua_getglobal(L, name);
        const double v = lua_tonumber(L, -1);
        lua_pop(L, 1);
        return v;
    };
    CHECK(global("with_group") == 5000);
    CHECK(global("hidden") == 5000);
    CHECK(global("with_child") == 7000);
    CHECK(global("destroyed") == 0);
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

TEST_CASE("A press no control eats reaches each control under the mouse once", "[ui][lua][input]") {
    InputFixture f;
    f.run("item = box('item', GetFrame(0), 0, 0, 100, 100, 1) "
          "label = box('label', item, 0, 0, 100, 20, 2)");
    f.dispatch.on_cursor_pos(10, 10);
    f.dispatch.on_mouse_button(GLFW_MOUSE_BUTTON_LEFT, GLFW_PRESS, 0);
    f.deliver();
    CHECK(f.check("whos('ButtonPress') == 'label,item'"));
}

TEST_CASE("DisableHitTest(true) lets the mouse through a control's children too",
          "[ui][lua][input]") {
    InputFixture f;
    f.run("button = box('button', GetFrame(0), 0, 0, 100, 100, 11) "
          "brackets = box('brackets', GetFrame(0), 0, 0, 100, 100, 12) "
          "corner = box('corner', brackets, 0, 0, 100, 100, 12) "
          "brackets:DisableHitTest(true)");
    f.dispatch.on_cursor_pos(50, 50);
    f.deliver();
    CHECK(f.check("whos('MouseEnter') == 'button'"));
}

namespace {

/// Test-mode failure counting, on for a scope (and the tally cleared).
struct CountingLuaFailures {
    CountingLuaFailures() {
        osc::test_status::reset();
        osc::test_status::set_count_lua_failures(true);
    }
    ~CountingLuaFailures() {
        osc::test_status::set_count_lua_failures(false);
        osc::test_status::reset();
    }
    CountingLuaFailures(const CountingLuaFailures&) = delete;
    CountingLuaFailures& operator=(const CountingLuaFailures&) = delete;
};

bool reported(const std::string& what, const std::string& error) {
    for (const auto& m : osc::test_status::failure_messages())
        if (m.find(what) != std::string::npos && m.find(error) != std::string::npos) return true;
    return false;
}

} // namespace

TEST_CASE("A dragger's script errors are reported, as other UI callbacks' are",
          "[ui][lua][input]") {
    // Retail's Button clicks through the Dragger it posts: an error there
    // (the click's own handler, say) must not vanish.
    InputFixture f;
    CountingLuaFailures counting;
    f.run(R"(
        moves = 0
        PostDragger(GetFrame(0), 1, {
            OnMove = function() moves = moves + 1 error('moving') end,
            OnRelease = function() error('letting go') end,
        })
    )");
    f.dispatch.on_cursor_pos(10, 10);
    f.dispatch.on_mouse_button(GLFW_MOUSE_BUTTON_LEFT, GLFW_RELEASE, 0);
    f.deliver();
    CHECK(f.check("moves == 1"));
    CHECK(reported("Dragger OnMove", "moving"));
    CHECK(reported("Dragger OnRelease", "letting go"));
    CHECK(osc::test_status::failure_count() == 2);

    // Let go all the same: the next release is the controls' again.
    f.run("screen = box('screen', GetFrame(0), 0, 0, 100, 100, 1) handled = {}");
    f.dispatch.on_mouse_button(GLFW_MOUSE_BUTTON_LEFT, GLFW_RELEASE, 0);
    f.deliver();
    CHECK(f.check("whos('ButtonRelease') == 'screen'"));

    f.run("PostDragger(GetFrame(0), 1, { OnCancel = function() error('called off') end })");
    f.dispatch.on_key(GLFW_KEY_ESCAPE, GLFW_PRESS, 0);
    f.deliver();
    CHECK(reported("Dragger OnCancel", "called off"));
    CHECK(osc::test_status::failure_count() == 3);
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

TEST_CASE("Only the Edit with the focus has it, as it moves between them", "[ui][lua][input]") {
    InputFixture f;
    lua_State* L = f.lua.raw();
    f.run(R"(
        function edit()
            local e = setmetatable({}, { __index = moho.edit_methods })
            InternalCreateEdit(e, GetFrame(0))
            return e
        end
        nickname = edit() address = edit() port = edit()
    )");
    auto* nickname = control_of(L, "nickname");
    auto* address = control_of(L, "address");
    auto* port = control_of(L, "port");
    REQUIRE(nickname);
    REQUIRE(address);
    REQUIRE(port);
    const auto focused = [&] {
        return std::vector<bool>{nickname->has_keyboard_focus(), address->has_keyboard_focus(),
                                 port->has_keyboard_focus()};
    };

    // None draws a caret until one takes the focus
    CHECK(focused() == std::vector<bool>{false, false, false});
    f.run("nickname:AcquireFocus()");
    CHECK(focused() == std::vector<bool>{true, false, false});
    // Taken by another, by either of Moho's calls: the first one loses it
    f.run("address:AcquireFocus()");
    CHECK(focused() == std::vector<bool>{false, true, false});
    f.run("moho.control_methods.AcquireKeyboardFocus(port, false)");
    CHECK(focused() == std::vector<bool>{false, false, true});
    // Let go by one without it: the focus stays
    f.run("nickname:AbandonFocus()");
    CHECK(focused() == std::vector<bool>{false, false, true});
    f.run("moho.control_methods.AbandonKeyboardFocus(port)");
    CHECK(focused() == std::vector<bool>{false, false, false});
    CHECK(f.registry.keyboard_focus() == nullptr);

    // A field clicked into takes the focus from the one that had it; one
    // whose script takes the press does not
    f.run("nickname:AcquireFocus()");
    f.run(R"(
        function place(e, l, t, r, b)
            rawset(e, 'Left', l) rawset(e, 'Top', t) rawset(e, 'Right', r) rawset(e, 'Bottom', b)
            rawset(e, 'Width', r - l) rawset(e, 'Height', b - t) rawset(e, 'Depth', 1)
        end
        place(address, 0, 0, 100, 20) place(port, 0, 30, 100, 50)
        port.HandleEvent = function(self, event) return event.Type == 'ButtonPress' end
    )");
    f.dispatch.on_cursor_pos(50, 10);
    f.dispatch.on_mouse_button(GLFW_MOUSE_BUTTON_LEFT, GLFW_PRESS, 0);
    f.dispatch.on_mouse_button(GLFW_MOUSE_BUTTON_LEFT, GLFW_RELEASE, 0);
    f.deliver();
    CHECK(focused() == std::vector<bool>{false, true, false});
    CHECK(f.registry.keyboard_focus() == address);
    f.dispatch.on_cursor_pos(50, 40);
    f.dispatch.on_mouse_button(GLFW_MOUSE_BUTTON_LEFT, GLFW_PRESS, 0);
    f.deliver();
    CHECK(focused() == std::vector<bool>{false, true, false});
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

TEST_CASE("A scrollbar's thumb spans the part of its scrollable shown", "[ui][scroll]") {
    using osc::ui::thumb_span;
    // (rangeMin, rangeMax, visibleMin, visibleMax), as Moho's GetScrollValues
    CHECK(thumb_span({0, 10, 0, 10}, 200, 16).length == 200); // all of it shows
    CHECK(thumb_span({0, 0, 0, 0}, 200, 16).length == 200);   // nothing to scroll
    const auto first = thumb_span({0, 20, 0, 5}, 200, 16);
    CHECK(first.start == 0);
    CHECK(first.length == 50);
    CHECK(thumb_span({0, 20, 10, 15}, 200, 16).start == 100);
    CHECK(thumb_span({0, 20, 15, 20}, 200, 16).start == 150);
    // No shorter than its caps, and still on its track at the end
    const auto last = thumb_span({0, 1000, 999, 1000}, 200, 16);
    CHECK(last.length == 16);
    CHECK(last.start == 184);
}

TEST_CASE("Along its track, a press finds the thumb, and a dragged thumb picks the top",
          "[ui][scroll]") {
    using osc::ui::dragged_top;
    using osc::ui::track_part;
    using osc::ui::TrackPart;
    const osc::ui::ThumbSpan thumb{50, 50};
    CHECK(track_part(thumb, 49) == TrackPart::Before);
    CHECK(track_part(thumb, 50) == TrackPart::Thumb);
    CHECK(track_part(thumb, 99) == TrackPart::Thumb);
    CHECK(track_part(thumb, 100) == TrackPart::After);

    // 40 rows, 10 shown, on a 200 track: the thumb's 150 of travel spans
    // tops 0 to 30, and goes no further either way
    const osc::ui::ScrollValues values{0, 40, 10, 20};
    CHECK(dragged_top(values, 200, 50, 0) == 0);
    CHECK(dragged_top(values, 200, 50, 75) == 15);
    CHECK(dragged_top(values, 200, 50, 150) == 30);
    CHECK(dragged_top(values, 200, 50, 400) == 30);
    CHECK(dragged_top(values, 200, 50, -20) == 0);
    // All of it shows: nothing to drag
    CHECK(dragged_top({0, 10, 0, 10}, 200, 200, 30) == 0);
}

TEST_CASE("A press on a scrollbar's track pages once, and its thumb drags the top shown",
          "[ui][lua][scroll][input]") {
    InputFixture f;
    // 40 rows, 10 shown from row 10, on a 200 high bar at y 100: its thumb
    // is 50 long at 150..200
    f.run(R"(
        pages = {} tops = {} top = 10
        list = setmetatable({
            GetScrollValues = function(self, axis) return 0, 40, top, top + 10 end,
            ScrollPages = function(self, axis, delta) table.insert(pages, delta) end,
            ScrollSetTop = function(self, axis, t) table.insert(tops, t) end,
        }, { __index = moho.control_methods })
        InternalCreateGroup(list, GetFrame(0))
        bar = setmetatable({}, { __index = moho.scrollbar_methods })
        InternalCreateScrollbar(bar, GetFrame(0), 'Vert')
        rawset(bar, 'Left', 300) rawset(bar, 'Top', 100)
        rawset(bar, 'Right', 320) rawset(bar, 'Bottom', 300)
        rawset(bar, 'Width', 20) rawset(bar, 'Height', 200) rawset(bar, 'Depth', 5)
        bar:SetScrollable(list)
    )");
    const auto press = [&](double y) {
        f.dispatch.on_cursor_pos(310, y);
        f.dispatch.on_mouse_button(GLFW_MOUSE_BUTTON_LEFT, GLFW_PRESS, 0);
        f.deliver();
    };
    const auto release = [&] {
        f.dispatch.on_mouse_button(GLFW_MOUSE_BUTTON_LEFT, GLFW_RELEASE, 0);
        f.deliver();
    };
    const auto move = [&](double y) {
        f.dispatch.on_cursor_pos(310, y);
        f.deliver();
    };

    // Above the thumb a page up, below it a page down; holding adds none
    press(120);
    move(121);
    release();
    press(260);
    release();
    CHECK(f.check("table.concat(pages, ',') == '-1,1' and table.getn(tops) == 0"));

    // The thumb, taken 10 into it, follows the mouse: its start at 100 of
    // its 150 travel is top 20; past the end, the last top
    press(160);
    move(210);
    move(400);
    release();
    move(250); // let go: no longer dragged
    CHECK(f.check("table.concat(tops, ',') == '20,30' and table.getn(pages) == 2"));
}

TEST_CASE("A scrollbar scrolls an ItemList, which keeps its own place", "[ui][lua][scroll]") {
    osc::lua::LuaState lua;
    osc::sim::SimState sim(lua.raw(), nullptr);
    osc::ui::UIControlRegistry registry;
    osc::lua::register_moho_bindings(lua, sim);
    osc::lua::register_ui_bindings(lua, registry);

    // Five rows show: 90 high, at the 14-point font's 18 (no font file here)
    auto result = lua.do_string(R"(
        list = setmetatable({}, { __index = moho.item_list_methods })
        InternalCreateItemList(list, GetFrame(0))
        list:SetNewFont('Arial', 14)
        rawset(list, 'Height', 90)
        for i = 1, 20 do list:AddItem('row ' .. i) end
        bar = setmetatable({}, { __index = moho.scrollbar_methods })
        InternalCreateScrollbar(bar, GetFrame(0), 'Vert')
        bar:SetScrollable(list)
        short = setmetatable({}, { __index = moho.item_list_methods })
        InternalCreateItemList(short, GetFrame(0))
        short:SetNewFont('Arial', 14)
        rawset(short, 'Height', 90)
        for i = 1, 3 do short:AddItem('row ' .. i) end
    )");
    INFO((result.ok() ? std::string() : result.error().message));
    REQUIRE(result.ok());

    lua_State* L = lua.raw();
    auto* list = control_of(L, "list");
    auto* bar = control_of(L, "bar");
    REQUIRE(list);
    REQUIRE(bar);
    const auto run = [&](const char* code) { REQUIRE(lua.do_string(code).ok()); };
    const auto shown = [&] {
        const auto v = osc::ui::scroll_values(L, *bar);
        return std::vector<float>{v.range_min, v.range_max, v.visible_min, v.visible_max};
    };
    const auto needs = [&](const char* name) {
        run((std::string("needs = ") + name + ":NeedsScrollBar()").c_str());
        lua_getglobal(L, "needs");
        const bool v = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        return v;
    };

    CHECK(needs("list"));
    CHECK_FALSE(needs("short"));
    CHECK(shown() == std::vector<float>{0, 20, 0, 5});
    run("bar:DoScrollLines(3)");
    CHECK(shown() == std::vector<float>{0, 20, 3, 8});
    run("bar:DoScrollPages(1)");
    CHECK(list->scroll_top() == 8);
    run("bar:DoScrollLines(100)"); // no further than its last row
    CHECK(shown() == std::vector<float>{0, 20, 15, 20});
    run("bar:DoScrollLines(-100)");
    CHECK(list->scroll_top() == 0);
    run("list:ScrollToBottom()");
    CHECK(list->scroll_top() == 15);
    run("list:ShowItem(2)"); // above: it becomes the top row
    CHECK(list->scroll_top() == 2);
    run("list:ShowItem(9)"); // below: it becomes the bottom row
    CHECK(list->scroll_top() == 5);
    // A dragged thumb's top: the nearest row, within its rows
    osc::ui::scroll_set_top(L, *bar, 7.4f);
    CHECK(list->scroll_top() == 7);
    osc::ui::scroll_set_top(L, *bar, 100);
    CHECK(list->scroll_top() == 15);
}

TEST_CASE("An ItemList takes a press on a row and tells of the row under the mouse",
          "[ui][lua][input]") {
    InputFixture f;
    // A Combo's list over the Combo, as retail's combo.lua lays them: rows
    // 18 high at the 14-point font (no font file here), from the fourth.
    f.run(R"(
        combo = box('combo', GetFrame(0), 0, 0, 100, 200, 1)
        combo.eats = true
        list = setmetatable({}, { __index = moho.item_list_methods })
        InternalCreateItemList(list, combo)
        list:SetNewFont('Arial', 14)
        rawset(list, 'Left', 0) rawset(list, 'Top', 0)
        rawset(list, 'Right', 100) rawset(list, 'Bottom', 90)
        rawset(list, 'Width', 100) rawset(list, 'Height', 90)
        rawset(list, 'Depth', 2)
        for i = 1, 20 do list:AddItem('row ' .. i) end
        list:ScrollToBottom() list:ShowItem(3)
        list.HandleEvent = function(self, event)
            table.insert(handled, { who = 'list', event = event })
            return self.eats
        end
        clicks = {}
        list.OnClick = function(self, row, event)
            table.insert(clicks, row .. ':' .. event.Type)
        end
        over = {}
        list.OnMouseoverItem = function(self, row) table.insert(over, row) end
    )");
    REQUIRE(control_of(f.lua.raw(), "list")->scroll_top() == 3);

    // Over its third shown row; along it, no news; off its side, none
    f.dispatch.on_cursor_pos(50, 40);
    f.dispatch.on_cursor_pos(60, 45);
    f.deliver();
    CHECK(f.check("table.concat(over, ',') == '5'"));

    // A press there: its row, the Combo under it not told
    f.dispatch.on_mouse_button(GLFW_MOUSE_BUTTON_LEFT, GLFW_PRESS, 0);
    f.dispatch.on_mouse_button(GLFW_MOUSE_BUTTON_LEFT, GLFW_RELEASE, 0);
    f.deliver();
    CHECK(f.check("table.concat(clicks, ',') == '5:ButtonPress'"));
    CHECK(f.check("whos('ButtonPress') == 'list'"));

    // One its script takes is the script's
    f.run("handled = {} clicks = {} list.eats = true");
    f.dispatch.on_mouse_button(GLFW_MOUSE_BUTTON_LEFT, GLFW_PRESS, 0);
    f.deliver();
    CHECK(f.check("table.getn(clicks) == 0 and whos('ButtonPress') == 'list'"));

    // Off its rows: -1, and a press goes on to the Combo
    f.run("handled = {} list.eats = nil");
    f.dispatch.on_cursor_pos(50, 150);
    f.dispatch.on_mouse_button(GLFW_MOUSE_BUTTON_LEFT, GLFW_PRESS, 0);
    f.deliver();
    CHECK(f.check("table.concat(over, ',') == '5,-1'"));
    CHECK(f.check("table.getn(clicks) == 0 and whos('ButtonPress') == 'combo'"));
}

TEST_CASE("A scrollbar asks a scrollable made in Lua for its values and scrolling",
          "[ui][lua][scroll]") {
    osc::lua::LuaState lua;
    osc::sim::SimState sim(lua.raw(), nullptr);
    osc::ui::UIControlRegistry registry;
    osc::lua::register_moho_bindings(lua, sim);
    osc::lua::register_ui_bindings(lua, registry);

    auto result = lua.do_string(R"(
        scrolled = 0
        group = setmetatable({
            GetScrollValues = function(self, axis) return 0, 30, 6, 16 end,
            ScrollLines = function(self, axis, delta) scrolled = scrolled + delta end,
        }, { __index = moho.control_methods })
        InternalCreateGroup(group, GetFrame(0))
        bar = setmetatable({}, { __index = moho.scrollbar_methods })
        InternalCreateScrollbar(bar, GetFrame(0), 'Vert')
        bar:SetScrollable(group)
        bar:DoScrollLines(2)
    )");
    INFO((result.ok() ? std::string() : result.error().message));
    REQUIRE(result.ok());

    lua_State* L = lua.raw();
    const auto v = osc::ui::scroll_values(L, *control_of(L, "bar"));
    CHECK(v.range_min == 0);
    CHECK(v.range_max == 30);
    CHECK(v.visible_min == 6);
    CHECK(v.visible_max == 16);
    lua_getglobal(L, "scrolled");
    CHECK(lua_tonumber(L, -1) == 2);
    lua_pop(L, 1);
}

TEST_CASE("A control's class OnFrame and OnLoseKeyboardFocus are called, as its own would be",
          "[ui][lua]") {
    InputFixture f;
    f.run(R"(
        frames, lost = 0, 0
        local Class = setmetatable({
            OnFrame = function(self, dt) frames = frames + 1 end,
            OnLoseKeyboardFocus = function(self) lost = lost + 1 end,
        }, { __index = moho.control_methods })
        a = setmetatable({}, { __index = Class })
        InternalCreateGroup(a, GetFrame(0))
        a:SetNeedsFrameUpdate(true)
        b = box('b', GetFrame(0), 0, 0, 10, 10, 1)
        a:AcquireKeyboardFocus(false)
        b:AcquireKeyboardFocus(false)
    )");
    f.dispatch.update_controls(f.lua.raw(), f.registry, 0.1);
    CHECK(f.check("frames == 1"));
    CHECK(f.check("lost == 1"));
}

TEST_CASE("An Edit's text is edited by character, at its caret", "[ui][edit]") {
    osc::ui::EditText t;
    CHECK(t.insert('a', 0));
    CHECK(t.insert(0xE9, 0));
    CHECK(t.insert('b', 0));
    CHECK(t.text == "a\xC3\xA9"
                    "b");
    t.left();
    CHECK(t.erase_before());
    CHECK(t.text == "ab");
    CHECK(t.caret == 1);
    CHECK(t.erase_after());
    CHECK(t.text == "a");
    CHECK_FALSE(t.erase_after());
    t.home();
    CHECK(t.insert('x', 2));
    CHECK_FALSE(t.insert('y', 2));
    CHECK(t.text == "xa");
    t.end();
    CHECK(t.caret == 2);
}

TEST_CASE("A focused Edit takes typed text and calls its On* methods, as Moho's does",
          "[ui][lua][input][edit]") {
    InputFixture f;
    f.run(R"(
        calls = {}
        local Class = setmetatable({
            OnTextChanged = function(self, new, old) table.insert(calls, 'changed ' .. new .. '<' .. old) end,
            OnCharPressed = function(self, c) table.insert(calls, 'char ' .. c) return c == 63 end,
            OnEnterPressed = function(self, text) table.insert(calls, 'enter ' .. text) end,
            OnEscPressed = function(self, text) table.insert(calls, 'esc ' .. text) return false end,
            OnNonTextKeyPressed = function(self, key, event)
                table.insert(calls, 'key ' .. key .. (event.Modifiers.Shift and ' shift' or ''))
            end,
        }, { __index = moho.edit_methods })
        e = setmetatable({}, { __index = Class })
        InternalCreateEdit(e, GetFrame(0))
    )");
    f.registry.set_keyboard_focus(control_of(f.lua.raw(), "e"));
    f.dispatch.on_char('h');
    f.dispatch.on_char('?');
    f.dispatch.on_char('i');
    f.dispatch.on_key(GLFW_KEY_LEFT, GLFW_PRESS, 0);
    f.dispatch.on_key(GLFW_KEY_BACKSPACE, GLFW_PRESS, 0);
    f.dispatch.on_key(GLFW_KEY_UP, GLFW_PRESS, GLFW_MOD_SHIFT);
    f.dispatch.on_key(GLFW_KEY_ENTER, GLFW_PRESS, 0);
    f.dispatch.on_key(GLFW_KEY_ESCAPE, GLFW_PRESS, 0);
    f.deliver();
    f.registry.set_keyboard_focus(nullptr);
    CHECK(f.check("table.concat(calls, ';') == 'char 104;changed h<;char 63;char 105;changed hi<h;"
                  "changed i<hi;key 38 shift;enter i;esc i;changed <i'"));
    CHECK(f.check("e:GetText() == ''"));
}

TEST_CASE("A press reaches uimain's OnMouseButtonPress, a module function", "[ui][lua][input]") {
    InputFixture f;
    f.run("target = box('target', GetFrame(0), 0, 0, 100, 100, 1) "
          "__modules = __modules or {} "
          "__modules['/lua/ui/uimain.lua'] = { OnMouseButtonPress = function(e) pressed = e.Type "
          "end }");
    f.dispatch.on_cursor_pos(50, 50);
    f.dispatch.on_mouse_button(GLFW_MOUSE_BUTTON_LEFT, GLFW_PRESS, 0);
    f.deliver();
    CHECK(f.check("pressed == 'ButtonPress'"));
}
