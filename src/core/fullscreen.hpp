#pragma once

#include <optional>

namespace osc::core {

/// macOS's full screen is the window's own Space (the green button's): a
/// display mode change there re-lays out the whole desktop.
#ifdef __APPLE__
inline constexpr bool kNativeFullscreen = true;
#else
inline constexpr bool kNativeFullscreen = false;
#endif

struct NativeFullscreenStep {
    bool toggle = false;
    bool apply_windowed = false;
    bool settled = false;
    bool operator==(const NativeFullscreenStep&) const = default;
};

inline NativeFullscreenStep native_fullscreen_step(std::optional<bool> want, bool in_fullscreen,
                                                   bool moving, bool windowed_pending) {
    if (moving) {
        return {};
    }
    if (want && *want != in_fullscreen) {
        return {.toggle = true};
    }
    return {.apply_windowed = !in_fullscreen && windowed_pending, .settled = true};
}

} // namespace osc::core
