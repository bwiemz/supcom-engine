#include "edit_text_test.hpp"

#include "app/app.hpp"
#include "app/window_commands.hpp"
#include "core/image.hpp"
#include "core/test_status.hpp"
#include "lua/lua_state.hpp"
#include "render_probe.hpp"
#include "renderer/renderer.hpp"
#include "ui/lazyvar.hpp"
#include "ui/ui_control.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

extern "C" {
#include <lua.h>
}

#include <algorithm>
#include <cstdlib>
#include <functional>

namespace osc::test {

namespace {

constexpr f64 kFrame = 1.0 / 30.0;
constexpr u32 kWidth = 1024;
constexpr u32 kHeight = 768;

struct Rect {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

Rect rect_of(lua_State* L, const ui::UIControl& c) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, c.lua_table_ref());
    const int t = lua_gettop(L);
    const Rect r{static_cast<int>(ui::read_lazyvar(L, t, "Left")),
                 static_cast<int>(ui::read_lazyvar(L, t, "Top")),
                 static_cast<int>(ui::read_lazyvar(L, t, "Right")),
                 static_cast<int>(ui::read_lazyvar(L, t, "Bottom"))};
    lua_pop(L, 1);
    return r;
}

int count_pixels(const ImageRGBA8& img, const Rect& r, const std::function<bool(const u8*)>& is) {
    int n = 0;
    for (int y = std::max(r.y0, 0); y < std::min(r.y1, static_cast<int>(img.height)); ++y) {
        for (int x = std::max(r.x0, 0); x < std::min(r.x1, static_cast<int>(img.width)); ++x) {
            if (is(&img.pixels[(static_cast<size_t>(y) * img.width + x) * 4])) {
                ++n;
            }
        }
    }
    return n;
}

ui::UIControl* find_control(app::Engine& e, const std::function<bool(const ui::UIControl&)>& is) {
    for (const auto& p : e.ui_registry.all()) {
        if (p && !p->destroyed() && is(*p)) {
            return p.get();
        }
    }
    return nullptr;
}

} // namespace

void run_edit_text_test(app::Engine& e) {
    spdlog::info("=== Edit text test ===");
    Tally t;
    lua_State* L = e.ui_lua_state.raw();
    renderer::Renderer r;
    if (!r.init(kWidth, kHeight, "Edit Text Test", /*offscreen=*/true)) {
        osc::test_status::fail("[FAIL] edit-text-test: no Vulkan device");
        return;
    }
    r.init_ui_caches(&e.vfs);
    r.set_fixed_frame_dt(static_cast<f32>(kFrame));
    app::size_root_frame(L, kWidth, kHeight);
    const auto shot = [&] {
        ImageRGBA8 image;
        r.request_capture([&](ImageRGBA8 captured) { image = std::move(captured); });
        r.render_ui_only(L, &e.ui_registry);
        r.render_ui_only(L, &e.ui_registry);
        return image;
    };

    if (auto res = e.ui_lua_state.do_string(
            "import('/lua/ui/dialogs/profile.lua').CreateDialog(function() end)");
        !res) {
        osc::test_status::fail("[FAIL] edit-text-test Lua: {}", res.error().message);
    }
    if (auto* label = find_control(
            e, [](const ui::UIControl& c) { return c.text_content() == "Create" && c.parent(); })) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, label->parent()->lua_table_ref());
        lua_setglobal(L, "__edit_text_create");
        if (auto res = e.ui_lua_state.do_string("__edit_text_create:OnClick()"); !res) {
            osc::test_status::fail("[FAIL] edit-text-test Lua: {}", res.error().message);
        }
    }
    auto* edit = find_control(e, [](const ui::UIControl& c) {
        return c.control_type() == ui::UIControl::ControlType::Edit && c.has_keyboard_focus();
    });
    if (!edit) {
        osc::test_status::fail("[FAIL] edit-text-test: the name dialog's Edit has no focus");
        r.shutdown();
        return;
    }

    for (u32 cp : {0xDCu, 0xDFu, 0x141u, 0x17Au, 0x10Cu, 0xE1u, 0xF1u, 0xE9u, 0x401u, 0x436u}) {
        r.ui_dispatch().on_char(cp);
    }
    const Rect field = rect_of(L, *edit);
    const u32 fg = edit->foreground_color();
    const auto is_text = [fg](const u8* p) {
        return std::abs(p[0] - static_cast<int>((fg >> 16) & 0xFF)) < 40 &&
               std::abs(p[1] - static_cast<int>((fg >> 8) & 0xFF)) < 40 &&
               std::abs(p[2] - static_cast<int>(fg & 0xFF)) < 40;
    };

    const ImageRGBA8 typed = shot();
    const int text_pixels = count_pixels(typed, field, is_text);
    t.check(edit->text_content() ==
                    "\xC3\x9C\xC3\x9F\xC5\x81\xC5\xBA\xC4\x8C\xC3\xA1\xC3\xB1\xC3\xA9"
                    "\xD0\x81\xD0\xB6" &&
                text_pixels > 40,
            fmt::format("Test 1: a name in accented Latin and Cyrillic letters typed into the name "
                        "dialog is drawn in the Edit "
                        "({} pixels of its {:08x})",
                        text_pixels, fg));

    r.shutdown();
    spdlog::info("Edit text test: {}/{} passed", t.pass, t.pass + t.fail);
}

} // namespace osc::test
