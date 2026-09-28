#pragma once

// The window's side of FA's video options (M217h): opening it as the
// command line and options say, the console commands the options screen
// runs, and what the options screen is told (the display's modes).

#include "app/window_mode.hpp"

#include <string>
#include <vector>

struct lua_State;

namespace osc::core {
class Preferences;
}
namespace osc::renderer {
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
void publish_adapter_options(lua_State* uL, const std::vector<Resolution>& modes, bool overridden);

/// SC_PrimaryAdapter, SC_SecondaryAdapter and SC_VerticalSync. They find
/// the renderer the UI state was given ("__osc_renderer").
void register_window_commands(ui::Console& console, core::Preferences& prefs, bool overridden);

/// The options' console variables and commands (M217i), Moho's TConVars
/// on the engine's tunables: cam_ZoomAmount, the ui_* pan and rotate speeds
/// and scroll switches, ui_AlwaysRenderStrategicIcons, ren_bloom,
/// ren_Skydome, graphics_Fidelity, shadow_Fidelity, ren_MipSkipLevels,
/// SC_CameraScaleLOD; and SC_AntiAliasingSamples and SC_ToggleCursorClip.
/// Without a renderer they read their defaults and set nothing.
void register_option_commands(ui::Console& console);

/// OPTIONS_Apply: optionsLogic.Apply(true), each option's set at startup,
/// as Moho runs it once the window is up.
void apply_options(lua_State* uL);

} // namespace osc::app
