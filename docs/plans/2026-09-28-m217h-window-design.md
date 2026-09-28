# M217h: The window, as FA's options set it

## Why

The engine's window is a fixed 1600×900. Its UI root frame is fixed at the
same size, it doesn't follow a resize, and vsync is always on. On a scaled
display (Wayland at 125%, 150%, 200%), every click lands off by the scale:
the cursor is read in window units and compared with framebuffer pixels.

FA's own options screen drives all of this through console commands and
options that Moho reads:
- `primary_adapter`: "windowed", or a `w,h,fps` mode for full screen;
- `SC_PrimaryAdapter`, which applies it at once;
- `vsync` and `SC_VerticalSync`;
- the mode list, which the engine publishes from the display.

This work is on this machine's path to a playable game (Wayland, a laptop
display).

The rules come from faf-re:
- `app/CScApp.cpp`: the startup head (`CreateAppFrame`'s context);
- `app/ResolutionCommands.cpp`: `SC_PrimaryAdapter`, `SC_VerticalSync`,
  `SC_ToggleCursorClip`;
- `misc/StartupHelpers.cpp`: `SetupPrimaryAdapterSettings`, the mode list,
  and the window size tunables;
- retail's `/lua/options/options.lua` and `optionsLogic.lua`: what the
  options screen expects.

## The rules

### At startup (CScApp)

- **The command line** wins over the options. Moho takes any of the
  prefixes `/`, `-`, `+` or `\`:
  - `windowed` (or `window`, `size`) `W H` is a window at W×H, at least
    1024×720 (`wnd_MinCmdLine*`);
  - `fullscreen W H` is full screen at W×H, at least 1024×768.
  - Either marks the adapter option "overridden".
  - `maximize` opens the window maximized.
  - `position X Y` places it.
- **Otherwise the `primary_adapter` option:**
  - "windowed" is a bordered, resizable window at the prefs'
    `Windows.Main.width`/`height` (default 1024×768, `wnd_DefaultCreate*`).
    It sits at `Windows.Main.x`/`y` and is maximized if
    `Windows.Main.maximized`.
  - A `w,h,fps` mode is full screen at that size.
- **vsync:** the `vsync` option (1 on).

### At runtime

- **`SC_PrimaryAdapter <value>`** (the options screen's Apply):
  - "windowed" restores the bordered window at the prefs' size;
  - `w,h,fps` goes borderless at that size;
  - "overridden" does nothing.
  - Either way the viewport follows.
- **`SC_VerticalSync <n>`:** Moho rebuilds the device with the option's
  vsync. It takes the value from the option, not the argument.
- **The window's place and size** go to `Windows.Main.*` as it moves or
  resizes while windowed, so the next start opens it there.

### The mode list (SetupPrimaryAdapterSettings)

- Without a command-line override, `primary_adapter`'s states are:
  - "Windowed" (`<LOC OPTIONS_0070>Windowed`, key "windowed");
  - then the display's modes, each at least 1024×768
    (`wnd_DefaultCreate*`), repeats left out, in the display's order,
    shown as `WxH(rate)` and keyed `W,H,rate`.
  - The default is "1024,768,60".
- With an override, the one state `<LOC _Command_Line_Override>`, key
  "overridden", is also the default.
- The engine publishes them through `optionsLogic.SetCustomData(key,
  custom, default)` (`OPTIONS_SetCustomData`). The options screen reads
  them back from the `options_overrides` pref (`GetOptionsData`).

## The engine

- **GLFW:**
  - Full screen is `glfwSetWindowMonitor` on the primary monitor at the
    mode's size and rate, which a Wayland compositor shows full screen.
  - Windowed is a decorated window at the prefs' size.
  - The modes are `glfwGetVideoModes`.
- **Resizing:** a framebuffer-size callback marks the swapchain stale, and
  the renderer rebuilds it before the next frame. It no longer waits for
  Vulkan to report it.
- **The UI root frame** is the framebuffer's size, set each time it
  changes. FA's layouts scale from it.
- **HiDPI:** cursor positions are scaled from window units to framebuffer
  pixels, everywhere they're read: the UI dispatch, the camera, world
  clicks, the minimap, `GetMouseWorldPos`.
- **vsync:** the present mode is FIFO with vsync on, and MAILBOX
  (IMMEDIATE if there's no MAILBOX) with it off.
- **Tests and captures** keep their fixed offscreen 1600×900. Options
  apply to a player's window only.

## Tests

- **Unit:**
  - the startup choice: the command line over the options, windowed at
    the prefs' size, a mode;
  - the mode list: the minimum, the repeats, the order, the keys and
    texts;
  - the `SC_PrimaryAdapter` parsing;
  - the cursor's scaling.
- **`--window-test` (gate):**
  - a hidden window follows `SC_PrimaryAdapter` (windowed at a size, the
    UI root frame the framebuffer's);
  - a click through the scaled cursor path lands on its pixel;
  - `SC_VerticalSync` rebuilds the swapchain with the present mode.
- **Manual:** on this machine's Wayland session, full screen, windowed,
  resize and the options screen's Apply.

## Left (M217i)

The options' other console variables: `ren_bloom`, `ren_Skydome`,
`graphics_Fidelity`, `shadow_Fidelity`, `SC_AntiAliasingSamples`,
`ren_MipSkipLevels`, `SC_CameraScaleLOD`, `cam_ZoomAmount`, the `ui_*` pan
and rotate speeds, and `SC_ToggleCursorClip`.
