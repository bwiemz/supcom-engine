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

/// The side mouse buttons as keys, as FAF's patched game has them
/// (FA-Binary-Patches OnWindowMessage.cpp makes WM_XBUTTONDOWN/UP the
/// WM_KEYDOWN/UP of VK_XBUTTON1/2, which wx passes on as key codes 5 and 6;
/// retail's wx 2.4 drops them). GLFW has no key below space, so these stand
/// for them among its key codes, with both codes 5 and 6.
constexpr i32 kKeyXButton1 = 5;
constexpr i32 kKeyXButton2 = 6;

/// The key a side mouse button is (kKeyXButton1/2), else 0.
i32 side_button_key(i32 glfw_button);

/// The GLFW keys a modifier's keyNames.lua name stands for, in any case;
/// none for another name.
std::vector<i32> glfw_keys_named(std::string_view name);

} // namespace osc::ui
