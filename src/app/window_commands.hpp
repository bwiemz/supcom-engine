#pragma once

// The window's side of FA's video options (M217h): opening it as the
// command line and options say, the console commands the options screen
// runs, and what the options screen is told (the display's modes).

#include "app/window_mode.hpp"

#include <map>
#include <string>
#include <vector>

struct lua_State;

namespace osc::core {
class Preferences;
}
namespace osc::renderer {
class RangeOverlays;
class Renderer;
}
namespace osc::ui {
class Console;
}

namespace osc::app {

/// What the startup choice reads: the primary_adapter option (the current
/// profile's, else options.lua's default) and Windows.Main.*.
WindowPrefs read_window_prefs(const core::Preferences& prefs);

/// The vsync option (the current profile's, else on).
bool vsync_option(const core::Preferences& prefs);

/// Open `r`'s window as CScApp does (the command line, else the options),
/// with the vsync option. Returns the mode it chose.
WindowMode open_window(renderer::Renderer& r, const std::vector<std::string>& args,
                       const core::Preferences& prefs);

/// Keep the window's place and size while windowed (Windows.Main.*), for
/// the next start and for SC_PrimaryAdapter "windowed".
void save_window_geometry(core::Preferences& prefs, const renderer::Renderer& r);

/// The UI's root frame: the framebuffer's size (FA's layouts scale from it).
void size_root_frame(lua_State* uL, u32 width, u32 height);

/// SetupPrimaryAdapterSettings and SetupSecondaryAdapterSettings: the
/// adapter options' states, through optionsLogic.SetCustomData.
void publish_adapter_options(lua_State* uL, const std::vector<Resolution>& modes, bool overridden,
                             const std::string& primary_adapter);

/// The primary_adapter option as a native full screen (macOS) was entered
/// or left, the green button's too.
void keep_adapter_option(lua_State* uL, core::Preferences& prefs, bool fullscreen);

/// The fidelity options' states and defaults, as retail's executable sets
/// them at startup: no Ultra preset, Medium by default, low shadows; and
/// antialiasing off only, for the engine draws without multisampling
void publish_fidelity_options(lua_State* uL);

/// SC_PrimaryAdapter, SC_SecondaryAdapter and SC_VerticalSync. They find
/// the renderer the UI state was given ("__osc_renderer").
void register_window_commands(ui::Console& console, core::Preferences& prefs, bool overridden);

/// What the console variables were set to before there was a renderer
/// (Moho's exist from startup; gamemain sets some before the window is up).
class HeldConVars {
public:
    void hold(const std::string& name, std::string value) { values_[name] = std::move(value); }
    const std::string* find(const std::string& name) const;
    void replay(ui::Console& console, lua_State* L);

private:
    std::map<std::string, std::string> values_;
};

/// The options' console variables and commands (M217i), Moho's TConVars
/// on the engine's tunables: cam_ZoomAmount, the ui_* pan and rotate speeds
/// and scroll switches, ui_AlwaysRenderStrategicIcons, ren_bloom,
/// ren_Skydome, ren_Skirt, graphics_Fidelity, shadow_Fidelity, ren_MipSkipLevels,
/// SC_CameraScaleLOD, the range_* convars; and SC_AntiAliasingSamples and
/// SC_ToggleCursorClip. Without a renderer they keep their values in `held`.
void register_option_commands(ui::Console& console, HeldConVars& held,
                              renderer::RangeOverlays& overlays);

/// OPTIONS_Apply: optionsLogic.Apply(true), each option's set at startup,
/// as Moho runs it once the window is up.
void apply_options(lua_State* uL);

} // namespace osc::app
