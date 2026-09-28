# M217i: FA's options, applied

## Why

FA's options screen applies every option through the console (`ConExecute
"cam_ZoomAmount 0.05"`, `"ui_KeyboardPanSpeed 90"`, `"ren_bloom true"`,
...). Moho's console variables bind those names to the engine's tunables.
The engine has console commands (M217c) and the window's (M217h), but no
variables. So every one of these logs "Unknown console command", and the
options do nothing.

Moho also applies the saved options at startup (`OPTIONS_Apply`:
`optionsLogic.Apply(true)`, called once the window is up). The engine never
does, so a player's volumes and pan speeds are the defaults each launch.

The rules come from faf-re:
- `console/CConCommand.cpp`: `TConVar<bool|int|float>` and their handlers;
- `app/CScApp.cpp`: `OPTIONS_Apply` and the cursor clip after it;
- `app/ResolutionCommands.cpp`: `SC_ToggleCursorClip`;
- retail's `/lua/options/options.lua`, for which options set which
  variables.

## The rules

### A console variable (TConVar)

- **bool:**
  - With no argument it toggles and logs "toggled X is now on/off".
  - `on`/`true` and `off`/`false` set it (in any case).
  - `tog` toggles.
  - `show` logs "bool X is on/off".
  - `= n` is n ≠ 0, and any other token is its integer ≠ 0.
- **int:**
  - `= += -= *= /= %= &= |=` then a value, or a bare value;
  - with no argument it logs "int X == v".
- **float:** as int, without `%= &= |=`; it logs "float X == %.4f".
- Values parse as `atoi`/`atof`: junk is 0.

### The options' variables and commands

| Option | Console | Engine |
|---|---|---|
| wheel_sensitivity | `cam_ZoomAmount` (float, 0.05) | the camera's zoom a notch |
| keyboard_pan_speed | `ui_KeyboardPanSpeed` (float, 90) | the arrows' pan |
| keyboard_pan_accelerate_multiplier | `ui_KeyboardPanAccelerateMultiplier` (float, 4) | with Ctrl |
| keyboard_rotate_speed | `ui_KeyboardRotateSpeed` (float, 10) | Insert/Delete |
| keyboard_rotate_accelerate_multiplier | `ui_KeyboardRotateAccelerateMultiplier` (float, 2) | with Ctrl |
| screen_edge_pans_main_view | `ui_ScreenEdgeScrollView` (bool) | the edges pan |
| arrow_keys_pan_main_view | `ui_ArrowKeysScrollView` (bool) | the arrows pan |
| strat_icons_always_on | `ui_AlwaysRenderStrategicIcons` (bool) | icons at every zoom |
| bloom_render | `ren_bloom` (bool) | the bloom pass |
| render_skydome | `ren_Skydome` (bool) | the sky dome (with #169) |
| fidelity | `graphics_Fidelity` (int) | kept |
| shadow_quality | `shadow_Fidelity` (int) | kept |
| texture_level | `ren_MipSkipLevels` (int) | kept |
| level_of_detail | `SC_CameraScaleLOD` | kept |
| antialiasing | `SC_AntiAliasingSamples` | kept (no MSAA yet) |
| lock_fullscreen_cursor_to_window | `SC_ToggleCursorClip` | the cursor held in the window |

The volumes go through `SetVolume`, which already works.

### At startup

- Once the window is up, `optionsLogic.Apply(true)` runs each option's
  `set(key, value, true)`.
- A windowed single head then clips the cursor if
  `lock_fullscreen_cursor_to_window` is 1.

### What changes for a player

At startup a player's saved options now apply, and so do FA's defaults
where nothing is saved:
- `wheel_sensitivity` 40 makes a wheel notch zoom by 2^-0.4, where the
  convar alone gave 2^-0.05;
- `bloom_render` 0 starts with bloom off;
- `texture_level` 1 keeps a mip skip of 1.

Captures, goldens and scripted checks don't apply the options: they keep
the engine's own values, so their frames don't change.

## Tests

- **Unit:**
  - each type's syntax: every operator, toggle, show, `on`/`off`/`tog`, a
    division by 0, junk;
  - a variable's binding through the console, case-insensitive;
  - the camera's settable wheel zoom, pan and rotate speeds and
    multipliers, and the arrow and edge switches.
- **`--options-test` (gate):**
  - a profile's saved options applied as at startup reach every tunable;
  - a player's `ConExecute`s with Moho's operators, and a bare name that
    only shows its value;
  - `SC_ToggleCursorClip` holds a window's cursor, and `0` lets go.
