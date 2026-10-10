#include "combo_click_test.hpp"

#include "app/app.hpp"
#include "app/window_commands.hpp"
#include "core/test_status.hpp"
#include "lua/lua_state.hpp"
#include "render_probe.hpp"
#include "renderer/renderer.hpp"
#include "ui/lazyvar.hpp"
#include "ui/ui_control.hpp"

#include <GLFW/glfw3.h>
#include <fmt/format.h>
#include <spdlog/spdlog.h>

extern "C" {
#include <lua.h>
}

namespace osc::test {

namespace {

constexpr u32 kWidth = 1024;
constexpr u32 kHeight = 768;

struct Box {
    f64 left = 0, top = 0, right = 0, bottom = 0;
};

Box box_of(lua_State* L, int ref) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    const int t = lua_gettop(L);
    const Box b{ui::read_lazyvar(L, t, "Left"), ui::read_lazyvar(L, t, "Top"),
                ui::read_lazyvar(L, t, "Right"), ui::read_lazyvar(L, t, "Bottom")};
    lua_pop(L, 1);
    return b;
}

bool lua_true(lua::LuaState& state, const char* expr) {
    lua_State* L = state.raw();
    if (auto r = state.do_string(fmt::format("__osc_cc_ok = ({}) and true or false", expr)); !r) {
        spdlog::error("combo-click-test Lua: {}", r.error().message);
        return false;
    }
    lua_getglobal(L, "__osc_cc_ok");
    const bool ok = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    return ok;
}

} // namespace

void run_combo_click_test(app::Engine& e) {
    spdlog::info("=== Combo click test ===");
    Tally t;
    lua_State* L = e.ui_lua_state.raw();
    renderer::Renderer r;
    if (!r.init(kWidth, kHeight, "Combo Click Test", /*offscreen=*/true)) {
        osc::test_status::fail("[FAIL] combo-click-test: no Vulkan device");
        return;
    }
    r.init_ui_caches(&e.vfs);
    app::size_root_frame(L, kWidth, kHeight);
    const auto frames = [&] {
        r.render_ui_only(L, &e.ui_registry);
        r.render_ui_only(L, &e.ui_registry);
    };
    auto& input = r.ui_dispatch();
    const auto click = [&](f64 x, f64 y) {
        input.on_cursor_pos(x, y);
        frames();
        input.on_mouse_button(GLFW_MOUSE_BUTTON_LEFT, GLFW_PRESS, 0);
        frames();
        input.on_mouse_button(GLFW_MOUSE_BUTTON_LEFT, GLFW_RELEASE, 0);
        frames();
    };

    if (auto res = e.ui_lua_state.do_string(R"(
            __osc_cc_closed = false
            import('/lua/ui/dialogs/options.lua').CreateDialog(GetFrame(0), function()
                __osc_cc_closed = true
            end)
            __osc_cc_combo = import('/lua/options/options.lua').options.gameplay.items[2].control
        )");
        !res) {
        osc::test_status::fail("[FAIL] combo-click-test Lua: {}", res.error().message);
        r.shutdown();
        return;
    }
    frames();
    const ui::UIControl* combo = nullptr;
    const ui::UIControl* cancel = nullptr;
    for (const auto& p : e.ui_registry.all()) {
        if (!p || p->destroyed() || p->lua_table_ref() < 0) {
            continue;
        }
        if (p->text_content() == "Cancel" && p->parent()) {
            cancel = p->parent();
        }
        lua_rawgeti(L, LUA_REGISTRYINDEX, p->lua_table_ref());
        lua_getglobal(L, "__osc_cc_combo");
        if (lua_rawequal(L, -1, -2)) {
            combo = p.get();
        }
        lua_pop(L, 2);
    }
    if (!combo || !cancel) {
        osc::test_status::fail("[FAIL] combo-click-test: the options dialog's combo {}, Cancel {}",
                               combo != nullptr, cancel != nullptr);
        r.shutdown();
        return;
    }

    const Box c = box_of(L, combo->lua_table_ref());
    click((c.left + c.right) / 2, (c.top + c.bottom) / 2);
    const bool opened = lua_true(e.ui_lua_state, "not __osc_cc_combo._dropdown:IsHidden()");
    click(8, 8);
    const bool closed = lua_true(e.ui_lua_state, "__osc_cc_combo._dropdown:IsHidden()");
    t.check(opened && closed && !lua_true(e.ui_lua_state, "__osc_cc_closed"),
            fmt::format("Test 1: a click on the combo opens its list ({}), one outside closes it "
                        "({}), and leaves the dialog open",
                        opened, closed));

    const Box b = box_of(L, cancel->lua_table_ref());
    click(b.left + 4, (b.top + b.bottom) / 2);
    t.check(lua_true(e.ui_lua_state, "__osc_cc_closed"),
            "Test 2: Cancel then takes its click, and closes the dialog");

    r.shutdown();
    spdlog::info("Combo click test: {}/{} passed", t.pass, t.pass + t.fail);
}

} // namespace osc::test
