#pragma once

// The keyboard's owner, as Moho moves it (MAUI_SetKeyboardFocus and the
// controls' LosingKeyboardFocus): the scripts are told as it changes.

struct lua_State;

namespace osc::ui {

class UIControl;
class UIControlRegistry;

/// Moho's MAUI_SetKeyboardFocus: `control` (or none) has the keyboard now,
/// and the control that had it is told, whatever it went to -- itself
/// too: its script's OnKeyboardFocusChange.
void set_keyboard_focus(lua_State* L, UIControlRegistry& registry, UIControl* control,
                        bool blocks_key_down);

/// Moho's CMauiControl::AbandonKeyboardFocus: none has the keyboard, if
/// `control` had it.
void abandon_keyboard_focus(lua_State* L, UIControlRegistry& registry, UIControl* control);

/// Moho's LosingKeyboardFocus, as a press goes to another control than the
/// focused one: an edit gives the keyboard up, then the control's script
/// hears OnLoseKeyboardFocus.
void losing_keyboard_focus(lua_State* L, UIControlRegistry& registry, UIControl* focused);

} // namespace osc::ui
