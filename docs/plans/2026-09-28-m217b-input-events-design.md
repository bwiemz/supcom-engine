# M217b: Moho's input events, and the input capture

Retail's scripts read input events in the form Moho gives them, and the
engine gave them GLFW's instead.

- **Key codes.** A key event's `KeyCode` was GLFW's, so Escape arrived as
  256 and Return as 257. Retail compares against wx's codes:
  `UIUtil.VK_ESCAPE` (27) and `VK_ENTER` (13) in its dialogs, 342-352 for
  the F-keys, 127 for Delete. None of those checks could match.
- **Mouse buttons.** A button event's `KeyCode` was GLFW's button (left 0,
  right 1). Moho gives wx's button numbers, left 1, middle 2 and right 3.
  The splash screen, the score screen, the credits and the campaign movies
  skip on `KeyCode == 1 or 3`. The events had no `Left`, `Middle` or
  `Right` modifiers either, which retail's click handlers test.
- **Input capture.** `AddInputCapture`, `RemoveInputCapture` and
  `AnyInputCapture` were stubs. Retail's splash screen, its movies and its
  modal dialogs capture input: while they are up, every click and key goes
  to them, and not to the hotkeys.

## Moho

From faf-re (`UiRuntimeTypes.cpp`, `EMauiKeyCodeTypeInfo.h`):

- **The event table** (`SMauiEventData` to Lua) holds `Type`, `MouseX`,
  `MouseY`, `WheelRotation`, `WheelDelta`, `KeyCode`, `RawKeyCode`,
  `Modifiers` and `Control`. The modifiers are `Shift`, `Ctrl` and `Alt`,
  plus `Left`, `Middle` and `Right` for a mouse event: the buttons held
  (wx's `m_leftDown`...). A press holds its own button; a release no
  longer does.
- **`KeyCode`** is wx's key code (`m_keyCode`, the `MKEY_` enum): ASCII for
  printable keys, 27, 13, 9, 8 and 127 for Escape, Return, Tab, Backspace
  and Delete, 300-396 for the rest (F1 342, Left 316, Up 317, Right 318,
  Down 319, the numpad 326-341). For a mouse button it is `GetButton()`,
  1-3. `RawKeyCode` is wx's `m_rawCode`, the Windows virtual-key code,
  which retail's key-binding dialog looks up in `keyNames.lua`.
- **The capture stack** holds weak links to controls. `AddInputCapture`
  pushes. `RemoveInputCapture` removes that control's last entry.
  `AnyInputCapture` and `GetInputCapture` read the top live one. A
  control's destruction removes it.
- **Mouse events** hit-test under the top capture instead of the root
  frame. A point over none of its children goes to the capture control
  itself.
- **Key events** go to the keyboard focus. With no focus they go to the
  top capture, and then no further: the event is not skipped, so the key
  map never sees it. With neither, the key map has it.

## The engine

- **`ui/key_codes`** maps GLFW to Moho: `moho_key_code` (wx),
  `windows_key_code` (the raw code) and `moho_mouse_button`. The Lua event
  table uses them. The C++ side keeps GLFW's codes for the key map's names
  and the dragger's Escape.
- **`UIDispatch`** tracks the buttons held and records them on each mouse
  event. The event table gains `RawKeyCode` and the button modifiers.
- **`UIControlRegistry`** holds the capture stack. Destroyed controls drop
  out of it when it is read, as Moho's weak links empty. `dispatch_events`
  hit-tests under the capture and hands it the keys as Moho does. The four
  globals are bound.

## Not done

- `WheelRotation` is still GLFW's scroll offset (1 a notch), not wx's 120.
  `WheelDelta` and `Control` are not given.
- Moho posts a double click as `ButtonDClick`. The engine sends two
  presses.
- Numpad Enter reports Return, as wx 2.4 on Windows does. The Windows keys
  have no wx code (0).

## Tests

- Unit: the key, raw key and button tables. The event tables for key and
  mouse events, including buttons held across events. The capture: clicks
  elsewhere go to it, its children come first, it takes the keys from the
  key map, focus comes first, the stack's order, destruction.
