#pragma once

#include "core/types.hpp"

#include <string_view>
#include <vector>

namespace osc::ui {

/// A key event's KeyCode as Moho gives it to Lua: wxWidgets' key code
/// (Moho's MKEY_ values). Printable keys keep their ASCII code, as GLFW's
/// do; the rest have wx's codes (Escape 27, Return 13, Delete 127, F1 342,
/// the arrows 316-319). 0 for a key wx has no code for.
i32 moho_key_code(i32 glfw_key);

/// A key event's RawKeyCode: the Windows virtual-key code (wx's raw code),
/// which retail's key-binding dialog looks up in keyNames.lua. 0 if none.
i32 windows_key_code(i32 glfw_key);

/// A mouse button event's KeyCode: wx's button number (left 1, middle 2,
/// right 3). 0 for any other button.
i32 moho_mouse_button(i32 glfw_button);

/// The GLFW keys a modifier's keyNames.lua name stands for, in any case;
/// none for another name.
std::vector<i32> glfw_keys_named(std::string_view name);

} // namespace osc::ui
