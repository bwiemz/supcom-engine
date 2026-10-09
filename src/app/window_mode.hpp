#pragma once

// The window as FA opens and sets it (M217h): Moho's startup head
// (CScApp), SC_PrimaryAdapter's argument, and the primary adapter option's
// states (SetupPrimaryAdapterSettings).

#include "core/types.hpp"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace osc::app {

/// Moho's window tunables: wnd_DefaultCreateWidth/Height (also the least a
/// listed mode may be) and wnd_MinCmdLineWidth/Height.
inline constexpr u32 kDefaultWidth = 1024;
inline constexpr u32 kDefaultHeight = 768;
inline constexpr u32 kMinCmdLineWidth = 1024;
inline constexpr u32 kMinCmdLineHeight = 720;
inline constexpr std::string_view kDefaultAdapterMode = "1024,768,60";

/// A display mode, or a primary_adapter option's "w,h,fps".
struct Resolution {
    u32 width = 0;
    u32 height = 0;
    u32 rate = 60;
    bool operator==(const Resolution&) const = default;
};

/// CFG_ParseResolutionTriple: "w,h,fps" (the rate may be left out), or
/// nothing for anything else ("windowed", "overridden", junk).
std::optional<Resolution> parse_resolution(std::string_view text);

/// How the window opens (CScApp's primary head).
struct WindowMode {
    bool fullscreen = false;
    Resolution size{kDefaultWidth, kDefaultHeight, 60};
    bool maximized = false;
    std::optional<std::array<i32, 2>> position;
    /// The command line chose it: the adapter option shows "overridden".
    bool overridden = false;
};

/// What the startup choice reads from the prefs: the primary_adapter
/// option (as prefs.lua's GetOption gives it: the profile's, else the
/// default) and the windowed geometry Moho saves (Windows.Main.*).
struct WindowPrefs {
    std::string primary_adapter{kDefaultAdapterMode};
    std::optional<u32> width, height;
    std::optional<i32> x, y;
    bool maximized = false;
};

/// The startup head: the command line's windowed/fullscreen/maximize/
/// position (any of Moho's prefixes /, -, +, \, or the engine's --), else
/// the primary_adapter option: "windowed" at the prefs' size and place, a
/// mode full screen.
WindowMode startup_window_mode(const std::vector<std::string>& args, const WindowPrefs& prefs);

/// One of an option's states, as options.lua lists them.
struct OptionState {
    std::string text;
    std::string key;
};

/// The primary_adapter option's states and default
/// (SetupPrimaryAdapterSettings): "Windowed", then the display's modes of
/// at least the default window size, repeats left out, in its order,
/// "WxH(rate)" keyed "W,H,rate", defaulting to "1024,768,60"; or, when the
/// command line chose the window, the override alone.
std::pair<std::vector<OptionState>, std::string>
adapter_states(const std::vector<Resolution>& modes, bool overridden);

/// macOS's full screen keeps the desktop's mode, so the option has one
/// full-screen state, keyed as the option holds it.
std::string native_fullscreen_key(std::string_view primary_adapter);
std::pair<std::vector<OptionState>, std::string>
native_adapter_states(std::string_view primary_adapter, bool overridden);
std::string adapter_option_for(bool fullscreen, std::string_view primary_adapter);

} // namespace osc::app
