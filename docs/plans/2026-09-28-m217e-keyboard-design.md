# M217e: retail's keys own the keyboard, and the world respects the capture

With retail's key map working (M217c, M217d), the engine's own keys fought
it:
- The camera panned on W, A, S and D. Retail binds W to zoom out, A to
  attack, S to stop and D to dive, so pressing S stopped the units and
  panned the camera. It panned while an edit box had the keyboard, too.
- The input handler kept its own control groups on the number keys:
  Ctrl+N set, N recalled, Shift+N and Ctrl+Shift+N were camera bookmarks.
  Retail binds 1–0 to its groups (`UI_ApplySelectionSet`), Ctrl to set
  them, Shift to append, and Ctrl+Shift to factory groups. Both acted on
  every press.
- B toggled bloom; retail binds it to build mode.
- A world click checked only whether a control was under the cursor. Under
  a modal dialog, or retail's opening lock, clicks on the world still
  selected units and issued orders.

## Moho

From faf-re `UiRuntimeTypes.cpp`:
- **The world view pans the camera** in its frame (`CUIWorldView`) on the
  arrow keys (`ui_ArrowKeysScrollView`) and at the screen edges
  (`ui_ScreenEdgeScrollView`), and spins it on Insert/Delete. It reads keys
  through `MAUI_KeyIsDown`, which is false while a control has focus or the
  window is in the background.
- **Letters and digits** belong to the key map.
- **Under an input capture**, the mouse hit-tests under the capture, never
  past it. The world view has the mouse only if it is in the capture.

## The engine

- **The camera** pans on the arrow keys only, and not while a control has
  focus (`Camera::set_keys_enabled`, set each frame from the registry's
  focus). The screen-edge scroll is unchanged.
- **The input handler's control groups and bookmarks are gone.** Retail's
  `UI_MakeSelectionSet`/`UI_ApplySelectionSet` (M217c) and
  `selection.lua` replace them.
- **The bloom key is gone.**
- **`UIDispatch::ui_has_mouse`** decides whether the UI or the world has the
  mouse. Under a capture it hit-tests under the capture, and only a world
  view there leaves the mouse to the world. The window's world clicks
  (`mouse_over_ui`) use it.

## Not done

- Insert/Delete spin.
- Moho's gate on the window being in the foreground (GLFW already sends no
  keys to a window in the background).

## Tests

- Unit: `ui_has_mouse` gives the mouse to a panel, to the world view, to a
  capture over it, and back to the world view when the capture holds it.
- `--keyboard-test` (gate): Ctrl-1 keeps the selection as retail's group 1,
  and 1 brings it back.
