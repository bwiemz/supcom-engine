// The window's side of FA's video options (M217h), from faf-re's CScApp
// (the startup head), ResolutionCommands (SC_PrimaryAdapter,
// SC_VerticalSync, SC_SecondaryAdapter) and StartupHelpers
// (SetupPrimaryAdapterSettings, SetupSecondaryAdapterSettings).

#include "app/window_commands.hpp"

#include "core/fullscreen.hpp"
#include "core/preferences.hpp"
#include "renderer/renderer.hpp"
#include "renderer/camera.hpp"
#include "ui/console.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <array>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

namespace osc::app {

namespace {

/// An option of the current profile's, as prefs.lua's GetOption reads it.
std::string option_key(const core::Preferences& prefs, const char* key) {
    const std::string profile = prefs.current_profile_path();
    return profile.empty() ? std::string() : profile + ".options." + key;
}

bool same_no_case(const std::string& a, const char* b) {
    const std::string_view bv(b);
    if (a.size() != bv.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(bv[i])))
            return false;
    return true;
}

renderer::Renderer* renderer_of(lua_State* L) {
    lua_pushstring(L, "__osc_renderer");
    lua_rawget(L, LUA_REGISTRYINDEX);
    auto* r = static_cast<renderer::Renderer*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return r;
}

/// Run `code` in the UI state, logging a failure.
void run(lua_State* L, const std::string& code, const char* what) {
    const int top = lua_gettop(L);
    if (luaL_loadbuffer(L, code.data(), code.size(), what) != 0 || lua_pcall(L, 0, 0, 0) != 0) {
        const char* err = lua_tostring(L, -1);
        spdlog::warn("{}: {}", what, err ? err : "(unknown)");
    }
    lua_settop(L, top);
}

/// optionsLogic.SetCustomData(key, {states = ...}, default)
/// (OPTIONS_SetCustomData). The texts and keys are the engine's own, with no
/// quotes to escape.
void set_custom_data(lua_State* L, const char* key, const std::vector<OptionState>& states,
                     const std::string& fallback) {
    std::string code = "import('/lua/options/optionsLogic.lua').SetCustomData('";
    code += key;
    code += "', {states = {";
    for (const OptionState& s : states)
        code += "{text = \"" + s.text + "\", key = \"" + s.key + "\"},";
    code += "}}, \"" + fallback + "\")";
    run(L, code, "publishing the adapter options");
}

} // namespace

WindowPrefs read_window_prefs(const core::Preferences& prefs) {
    WindowPrefs w;
    if (const std::string key = option_key(prefs, "primary_adapter"); !key.empty())
        w.primary_adapter = prefs.get_string(key, w.primary_adapter);
    if (const int width = prefs.get_int("Windows.Main.width", 0); width > 0)
        w.width = static_cast<u32>(width);
    if (const int height = prefs.get_int("Windows.Main.height", 0); height > 0)
        w.height = static_cast<u32>(height);
    const int x = prefs.get_int("Windows.Main.x", INT_MIN);
    const int y = prefs.get_int("Windows.Main.y", INT_MIN);
    if (x != INT_MIN && y != INT_MIN) {
        w.x = x;
        w.y = y;
    }
    w.maximized = prefs.get_bool("Windows.Main.maximized", false);
    return w;
}

bool vsync_option(const core::Preferences& prefs) {
    const std::string key = option_key(prefs, "vsync");
    return key.empty() || prefs.get_int(key, 1) == 1;
}

WindowMode open_window(renderer::Renderer& r, const std::vector<std::string>& args,
                       const core::Preferences& prefs) {
    const WindowMode m = startup_window_mode(args, read_window_prefs(prefs));
    if (m.fullscreen) {
        r.set_fullscreen(m.size.width, m.size.height, m.size.rate);
    } else {
        r.set_windowed(m.size.width, m.size.height, m.position, m.maximized);
    }
    r.set_vsync(vsync_option(prefs));
    spdlog::info("Window: {} {}x{}{}", m.fullscreen ? "full screen" : "windowed", m.size.width,
                 m.size.height, m.overridden ? " (the command line's)" : "");
    return m;
}

void save_window_geometry(core::Preferences& prefs, const renderer::Renderer& r) {
    const auto g = r.windowed_geometry();
    if (!g) return;
    prefs.set_bool("Windows.Main.maximized", g->maximized);
    if (g->maximized) return; // the size it had before is the one to keep
    prefs.set_int("Windows.Main.x", g->x);
    prefs.set_int("Windows.Main.y", g->y);
    prefs.set_int("Windows.Main.width", static_cast<int>(g->width));
    prefs.set_int("Windows.Main.height", static_cast<int>(g->height));
}

void size_root_frame(lua_State* uL, u32 width, u32 height) {
    const std::string w = std::to_string(width);
    const std::string h = std::to_string(height);
    run(uL,
        "local f = GetFrame(0)\n"
        "if f then\n"
        "  f.Left:Set(0)\n"
        "  f.Top:Set(0)\n"
        "  f.Width:Set(" +
            w +
            ")\n"
            "  f.Height:Set(" +
            h +
            ")\n"
            "  f.Right:Set(" +
            w +
            ")\n"
            "  f.Bottom:Set(" +
            h +
            ")\n"
            "end\n",
        "sizing the root frame");
}

void publish_adapter_options(lua_State* uL, const std::vector<Resolution>& modes, bool overridden,
                             const std::string& primary_adapter) {
    const auto [states, fallback] = core::kNativeFullscreen
                                        ? native_adapter_states(primary_adapter, overridden)
                                        : adapter_states(modes, overridden);
    set_custom_data(uL, "primary_adapter", states, fallback);
    // One display: the secondary adapter is disabled, or overridden
    if (overridden) {
        set_custom_data(uL, "secondary_adapter", {{"<LOC _Command_Line_Override>", "overridden"}},
                        "overridden");
    } else {
        set_custom_data(uL, "secondary_adapter", {{"<LOC _Disabled>", "disabled"}}, "disabled");
    }
}

void keep_adapter_option(lua_State* uL, core::Preferences& prefs, bool fullscreen) {
    const std::string key = option_key(prefs, "primary_adapter");
    if (key.empty()) {
        return;
    }
    const std::string held = prefs.get_string(key, std::string(kDefaultAdapterMode));
    const std::string now = adapter_option_for(fullscreen, held);
    if (now == held) {
        return;
    }
    prefs.set_string(key, now);
    publish_adapter_options(uL, {}, false, now);
}

void publish_fidelity_options(lua_State* uL) {
    run(uL,
        "local logic = import('/lua/options/optionsLogic.lua')\n"
        "logic.SetCustomData('fidelity_presets', {states = {\n"
        "    {text = '<LOC _Low>', key = 0}, {text = '<LOC _Medium>', key = 1},\n"
        "    {text = '<LOC _High>', key = 2}, {text = '<LOC _Custom>', key = 4}}}, 1)\n"
        "logic.SetCustomData('fidelity', {states = {\n"
        "    {text = '<LOC _Low>', key = 0}, {text = '<LOC _Medium>', key = 1},\n"
        "    {text = '<LOC _High>', key = 2}}}, 1)\n"
        "logic.SetCustomData('shadow_quality', {states = {\n"
        "    {text = '<LOC _Off>', key = 0}, {text = '<LOC _Low>', key = 1},\n"
        "    {text = '<LOC _Medium>', key = 2}, {text = '<LOC _High>', key = 3}}}, 1)\n"
        "logic.SetCustomData('antialiasing', {states = {\n"
        "    {text = '<LOC OPTIONS_0029>Off', key = 0}}}, 0)\n",
        "publishing the fidelity options");
}

void register_window_commands(ui::Console& console, core::Preferences& prefs, bool overridden) {
    using Args = std::vector<std::string>;
    // SC_PrimaryAdapter <windowed | w,h,fps>: the options screen's Apply.
    // "windowed" is the decorated window at the size it last had; a mode is
    // full screen at it; "overridden" leaves the command line's window.
    console.add("SC_PrimaryAdapter", [&prefs](lua_State* L, const Args& args) {
        if (args.size() != 2 || same_no_case(args[1], "overridden")) return;
        renderer::Renderer* r = renderer_of(L);
        if (!r) return;
        if (same_no_case(args[1], "windowed")) {
            const WindowPrefs w = read_window_prefs(prefs);
            std::optional<std::array<i32, 2>> place;
            if (w.x && w.y) place = std::array<i32, 2>{*w.x, *w.y};
            r->set_windowed(w.width.value_or(kDefaultWidth), w.height.value_or(kDefaultHeight),
                            place, w.maximized);
            return;
        }
        const auto mode = parse_resolution(args[1]);
        if (!mode) {
            spdlog::warn("SC_PrimaryAdapter: \"{}\" is no mode", args[1]);
            return;
        }
        save_window_geometry(prefs, *r); // for "windowed" again
        r->set_fullscreen(mode->width, mode->height, mode->rate);
    });
    // SC_SecondaryAdapter <bool>: republishes the secondary adapter's states
    // (one display here: disabled, or the command line's).
    console.add("SC_SecondaryAdapter", [overridden](lua_State* L, const Args& args) {
        if (args.size() != 2) return;
        if (overridden) {
            set_custom_data(L, "secondary_adapter",
                            {{"<LOC _Command_Line_Override>", "overridden"}}, "overridden");
        } else {
            set_custom_data(L, "secondary_adapter", {{"<LOC _Disabled>", "disabled"}}, "disabled");
        }
    });
    // SC_VerticalSync <n>: the device again, with the vsync option (Moho
    // reads the option, not the argument; the options screen has stored it)
    console.add("SC_VerticalSync", [&prefs](lua_State* L, const Args& args) {
        if (args.size() != 2) return;
        if (renderer::Renderer* r = renderer_of(L)) r->set_vsync(vsync_option(prefs));
    });
}

const std::string* HeldConVars::find(const std::string& name) const {
    const auto it = values_.find(name);
    return it == values_.end() ? nullptr : &it->second;
}

void HeldConVars::replay(ui::Console& console, lua_State* L) {
    const auto values = std::move(values_);
    values_.clear();
    for (const auto& [name, value] : values) {
        std::string line = name;
        line += ' ';
        line += value;
        console.execute(L, line);
    }
}

namespace {

template <class T> T parse_held(const std::string& text) {
    if constexpr (std::is_same_v<T, bool>) {
        return text == "true";
    } else if constexpr (std::is_same_v<T, int>) {
        return static_cast<int>(std::strtol(text.c_str(), nullptr, 10));
    } else {
        return std::strtof(text.c_str(), nullptr);
    }
}

template <class T>
void renderer_var(ui::Console& console, HeldConVars& held, const std::string& name, T fallback,
                  std::function<T(renderer::Renderer&)> get,
                  std::function<void(renderer::Renderer&, T)> set) {
    auto read = [&held, name, fallback, get = std::move(get)](lua_State* L) {
        if (renderer::Renderer* r = renderer_of(L)) {
            return get(*r);
        }
        const std::string* text = held.find(name);
        return text ? parse_held<T>(*text) : fallback;
    };
    auto write = [&held, name, set = std::move(set)](lua_State* L, T v) {
        if (renderer::Renderer* r = renderer_of(L)) {
            set(*r, v);
        } else {
            held.hold(name, fmt::format("{}", v));
        }
    };
    if constexpr (std::is_same_v<T, bool>) {
        ui::add_bool_var(console, name, std::move(read), std::move(write));
    } else if constexpr (std::is_same_v<T, int>) {
        ui::add_int_var(console, name, std::move(read), std::move(write));
    } else {
        ui::add_float_var(console, name, std::move(read), std::move(write));
    }
}

} // namespace

void register_option_commands(ui::Console& console, HeldConVars& held,
                              renderer::RangeOverlays& overlays) {
    using renderer::Camera;
    using renderer::RangeOverlays;
    using renderer::Renderer;
    const auto camera_float = [&](const char* name, f32 fallback, f32 (Camera::*get)() const,
                                  void (Camera::*set)(f32)) {
        renderer_var<f32>(
            console, held, name, fallback, [get](Renderer& r) { return (r.camera().*get)(); },
            [set](Renderer& r, f32 v) { (r.camera().*set)(v); });
    };
    camera_float("cam_ZoomAmount", Camera::kZoomAmount, &Camera::zoom_amount,
                 &Camera::set_zoom_amount);
    camera_float("ui_KeyboardPanSpeed", Camera::kKeyboardPanSpeed, &Camera::keyboard_pan_speed,
                 &Camera::set_keyboard_pan_speed);
    camera_float("ui_KeyboardPanAccelerateMultiplier", Camera::kKeyboardPanAccelerate,
                 &Camera::keyboard_pan_accelerate, &Camera::set_keyboard_pan_accelerate);
    camera_float("ui_KeyboardRotateSpeed", Camera::kKeyboardRotateSpeed,
                 &Camera::keyboard_rotate_speed, &Camera::set_keyboard_rotate_speed);
    camera_float("ui_KeyboardRotateAccelerateMultiplier", Camera::kKeyboardRotateAccelerate,
                 &Camera::keyboard_rotate_accelerate, &Camera::set_keyboard_rotate_accelerate);
    const auto camera_bool = [&](const char* name, bool fallback, bool (Camera::*get)() const,
                                 void (Camera::*set)(bool)) {
        renderer_var<bool>(
            console, held, name, fallback, [get](Renderer& r) { return (r.camera().*get)(); },
            [set](Renderer& r, bool v) { (r.camera().*set)(v); });
    };
    camera_bool("ui_ScreenEdgeScrollView", true, &Camera::edge_scroll, &Camera::set_edge_scroll);
    camera_bool("ui_ArrowKeysScrollView", true, &Camera::arrow_scroll, &Camera::set_arrow_scroll);
    // cam_Free: "Allow the camera to remain rotated" (off, as Moho starts)
    camera_bool("cam_Free", false, &Camera::free, &Camera::set_free);
    const auto renderer_bool = [&](const char* name, bool fallback, bool (Renderer::*get)() const,
                                   void (Renderer::*set)(bool)) {
        renderer_var<bool>(
            console, held, name, fallback, [get](Renderer& r) { return (r.*get)(); },
            [set](Renderer& r, bool v) { (r.*set)(v); });
    };
    renderer_bool("ui_AlwaysRenderStrategicIcons", false, &Renderer::icons_always,
                  &Renderer::set_icons_always);
    // What a campaign's NIS turns off and on again (gamemain.NISMode)
    renderer_bool("ui_RenderUnitBars", true, &Renderer::unit_bars, &Renderer::set_unit_bars);
    renderer_bool("ui_NisRenderIcons", true, &Renderer::nis_icons, &Renderer::set_nis_icons);
    renderer_bool("UI_forceWeaponsToYellow", true, &Renderer::weapons_yellow,
                  &Renderer::set_weapons_yellow);
    renderer_bool("ren_SelectBoxes", true, &Renderer::select_boxes, &Renderer::set_select_boxes);
    renderer_bool("ren_bloom", true, &Renderer::bloom_enabled, &Renderer::set_bloom_enabled);
    // ren_ShadowBlur: the High lane's five-tap shadows at shadow fidelity 3
    // (Moho's default on, M211m)
    const auto video_bool = [&](const char* name, bool Renderer::VideoOptions::* field) {
        renderer_var<bool>(
            console, held, name, Renderer::VideoOptions{}.*field,
            [field](Renderer& r) { return r.video_options().*field; },
            [field](Renderer& r, bool v) { r.video_options().*field = v; });
    };
    video_bool("ren_ShadowBlur", &Renderer::VideoOptions::shadow_blur);
    video_bool("ren_Skydome", &Renderer::VideoOptions::skydome);
    video_bool("ren_Skirt", &Renderer::VideoOptions::skirt);
    // The range overlays' (Moho's RangeRenderer convars; retail's UI sets
    // the first three from the player's prefs)
    const auto range_bool = [&](const char* name, bool RangeOverlays::Settings::* field) {
        ui::add_bool_var(
            console, name, [&overlays, field](lua_State*) { return overlays.settings().*field; },
            [&overlays, field](lua_State*, bool v) { overlays.settings().*field = v; });
    };
    range_bool("range_RenderSelected", &RangeOverlays::Settings::render_selected);
    range_bool("range_RenderHighlighted", &RangeOverlays::Settings::render_highlighted);
    range_bool("range_RenderBuild", &RangeOverlays::Settings::render_build);
    range_bool("range_Fill", &RangeOverlays::Settings::fill);
    range_bool("ren_Ranges", &RangeOverlays::Settings::enabled);
    const auto range_float = [&](const char* name, f32 RangeOverlays::Settings::* field) {
        ui::add_float_var(
            console, name, [&overlays, field](lua_State*) { return overlays.settings().*field; },
            [&overlays, field](lua_State*, f32 v) { overlays.settings().*field = v; });
    };
    range_float("range_InnerThicknessCoeff", &RangeOverlays::Settings::inner_thickness_coeff);
    range_float("range_OuterThicknessCoeff", &RangeOverlays::Settings::outer_thickness_coeff);
    // The ints the renderer keeps
    const auto video_int = [&](const char* name, int Renderer::VideoOptions::* field) {
        renderer_var<int>(
            console, held, name, Renderer::VideoOptions{}.*field,
            [field](Renderer& r) { return r.video_options().*field; },
            [field](Renderer& r, int v) { r.video_options().*field = v; });
    };
    video_int("graphics_Fidelity", &Renderer::VideoOptions::graphics_fidelity);
    video_int("shadow_Fidelity", &Renderer::VideoOptions::shadow_fidelity);
    video_int("ren_MipSkipLevels", &Renderer::VideoOptions::mip_skip_levels);
    renderer_var<f32>(
        console, held, "SC_CameraScaleLOD", Renderer::VideoOptions{}.camera_scale_lod,
        [](Renderer& r) { return r.video_options().camera_scale_lod; },
        [](Renderer& r, f32 v) { r.video_options().camera_scale_lod = v; });

    using Args = std::vector<std::string>;
    // SC_AntiAliasingSamples <packed>: kept (no multisampling yet)
    console.add("SC_AntiAliasingSamples", [&held](lua_State* L, const Args& args) {
        if (args.size() != 2) {
            return;
        }
        if (Renderer* r = renderer_of(L)) {
            r->video_options().antialiasing =
                static_cast<int>(std::strtol(args[1].c_str(), nullptr, 10));
        } else {
            held.hold(args[0], args[1]);
        }
    });
    // SC_ToggleCursorClip [0]: "0" lets the cursor go; anything else (or
    // nothing) holds it in a window
    console.add("SC_ToggleCursorClip", [&held](lua_State* L, const Args& args) {
        if (args.size() > 2) {
            return;
        }
        const bool clip = !(args.size() == 2 && args[1] == "0");
        if (Renderer* r = renderer_of(L)) {
            r->set_cursor_clip(clip);
        } else {
            held.hold(args[0], clip ? "1" : "0");
        }
    });
}

void apply_options(lua_State* uL) {
    run(uL, "import('/lua/options/optionsLogic.lua').Apply(true)", "applying the options");
}

} // namespace osc::app
