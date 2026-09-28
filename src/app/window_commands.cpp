// The window's side of FA's video options (M217h), from faf-re's CScApp
// (the startup head), ResolutionCommands (SC_PrimaryAdapter,
// SC_VerticalSync, SC_SecondaryAdapter) and StartupHelpers
// (SetupPrimaryAdapterSettings, SetupSecondaryAdapterSettings).

#include "app/window_commands.hpp"

#include "core/preferences.hpp"
#include "renderer/renderer.hpp"
#include "ui/console.hpp"

#include <spdlog/spdlog.h>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <array>
#include <cctype>
#include <climits>
#include <optional>
#include <string>
#include <string_view>

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

void publish_adapter_options(lua_State* uL, const std::vector<Resolution>& modes, bool overridden) {
    const auto [states, fallback] = adapter_states(modes, overridden);
    set_custom_data(uL, "primary_adapter", states, fallback);
    // One display: the secondary adapter is disabled, or overridden
    if (overridden) {
        set_custom_data(uL, "secondary_adapter", {{"<LOC _Command_Line_Override>", "overridden"}},
                        "overridden");
    } else {
        set_custom_data(uL, "secondary_adapter", {{"<LOC _Disabled>", "disabled"}}, "disabled");
    }
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

} // namespace osc::app
