# M217f: Moho's camera

## Why

The engine's camera is its own design:
- an orbit at a pitch clamped to 30–80°;
- a fixed 45° field of view;
- a zoom scaled 10% a wheel notch;
- pan by the frame's time;
- a middle-drag orbit.

FA's camera works differently:
- The zoom is the world extent in view.
- Zooming tilts the view, from top-down at the farthest zoom to 40° at the nearest.
- The field of view widens as the view closes in.
- The zoom glides in log space.
- The middle button drags the ground.
- Space (or Insert and Delete) spins the view, and letting go glides it back.

Every screen of the game frames differently from FA's until this matches.

The rules come from:
- faf-re's `CameraImpl`:
  - `UpdateBasis`, `UpdateCoords`, `CalculateFOV`, `GetMaxZoom`;
  - `CameraZoom`, `CameraPan`, `CameraSpin`, `CameraReset`;
  - `ClampTargetPos`, `ClampFocusPos`;
  - the Lua getters;
- `CUIWorldView::HandleEvent` and `Frame`, for the input;
- `CameraDragger`, for the middle-drag;
- `VEC_D3DProjectionMatrixFOV`;
- the tunables in `RuntimeTuningGlobals` and `UiRuntimeTypes`.

M217g follows with the timed moves: `MoveTo` and `SetZoom` over seconds, Hermite easing, `TargetEntities` and tracking.

## The rules

### Tunables

| Name | Value |
|---|---|
| `cam_NearZoom` | 5 |
| `cam_NearPitch` | 40° |
| `cam_FarPitch` | 89.9° |
| `cam_NearFOV` | 65° |
| `cam_FarFOV` | 60° |
| `cam_ZoomAmount` | 0.05 |
| `cam_ZoomSpeedLarge` | 8 |
| `cam_ZoomSpeedSmall` | 1 |
| `cam_SpinSpeed` | 360° |
| `cam_MinSpinPitch` | 0.1 rad |
| spin pitch ceiling | 1.5607964 rad |
| `cam_PanSpeed` | 1 |
| `mMaxZoomMult` | 1.4 |
| `ren_BorderSize` | 0 |
| `ren_BgLowerBound` | 125 |
| `ui_KeyboardPanSpeed` | 90 (×4 with Ctrl) |
| `ui_KeyboardRotateSpeed` | 10 (×2 with Ctrl) |

### The zoom

The zoom is the horizontal world extent at the focus.

- **The farthest zoom** is
  `max(width · mult, height · mult · aspect)` over the playable rect (the
  whole map without one), with `aspect` the viewport's width/height.
- **A wheel notch** (`rotation / wheelDelta`, ±1) calls
  `CameraZoom(n)`:
  - it first sets the pivot to the cursor;
  - then `nearZoom = 2^(−0.05 n) · nearZoom`, clamped to [5, max].
- **Each frame (`UpdateBasis`):**
  - The target zoom glides toward `nearZoom` in log₂ space, by at most
    `(|Δ| · 8 + 1) · dt`.
  - Unless the view is rotated, it is clamped to [5, max].
  - When zooming in (the zoom shrinking), the target is drawn toward the
    ground point under the pivot by `new / old`, so the view closes on the
    cursor. faf-re's comment says "zooming out", but its condition
    (`start > target`) is the zoom shrinking.
- **The target** is clamped into the rect, less
  `(zoom / max) · half` its extent each way (`ClampTargetPos`).

### The field of view and pitch

Both follow the log of the target zoom:

`f = (ln clamp(zoom, 5, max) − ln 5) / (ln max − ln 5)`

- The field of view is `65° + f · (60° − 65°)`.
- Unless the view is rotated, the pitch is `40° + f · (89.9° − 40°)`.

### The view (`UpdateCoords`)

- **The eye distance** is `zoom / tan(fov / 2) / 2`, which is what
  `CameraGetZoom` returns.
- **The focus** is the target, dropped along the view ray onto the
  terrain's or the water's surface (`ClampFocusPos`).
- **The eye** is `focus − d · dir`, where
  `dir = (sin h cos p, −sin p, cos h cos p)`.
  - A heading of π looks toward −z (north on the minimap).
- **Clip planes:** near is `max(0.01 d, 0.01)`, far is `d + 17000`.
- **Projection:** D3D's FOV projection with `fovX = fovY = fov` and
  `aspect > 1`. So the FOV is horizontal, and the vertical half-angle is
  `atan(tan(fov/2) / aspect)`.

### Moves

- **Pan (`CameraPan(dx, dy)`, in pixels):**
  - The target moves by `(zoom / viewportWidth) · cam_PanSpeed` a pixel.
  - It moves against the view's right for dx, and along its up, flattened
    onto the ground, for dy.
  - Uses:
    - the middle-button drag: `CameraDragger`, by the mouse's movement
      since the last move;
    - the keyboard: arrows and screen edges, 90 a frame (×4 with Ctrl).
      Up and the top edge are +dy; left and the left edge are +dx.
- **Spin (`CameraSpin(dx, dy)`, in pixels):**
  - The turn is `cam_SpinSpeed / viewportWidth` degrees a pixel: heading
    `−= dx`, pitch `+= dy`.
  - The pitch is clamped to [0.1, 1.5608].
  - It marks the view rotated.
  - Uses:
    - Space + mouse motion, only while `zoom < 125`;
    - Insert and Delete, ±10 a frame (×2 with Ctrl), not with Alt.
- **Revert (`CameraRevertRotation`):**
  - Triggered by letting go of Space, or by the end of a middle-drag.
  - Each frame the heading steps 0.1 toward ±π, and the pitch steps 0.1
    toward the zoom's pitch.
  - Once both are within 0.1, they snap, and the view is no longer rotated.
- **Reset (`CameraReset`):**
  - heading π, pitch 89.9°, FOV 60°;
  - the target at the map's centre, on the surface;
  - zoom = the farthest.

Frame order: `UpdateBasis`, then `UpdateCoords`.

## The engine

- **`renderer::Camera` is rewritten as `CameraImpl`'s basis:**
  - The lanes:
    - the target, the focus;
    - the heading and pitch, the fov;
    - the near and target zooms;
    - rotated, revert;
    - the pivot, the zoom multiplier;
    - the viewport, the playable rect.
  - Moho's operations: `zoom(n)`, `set_pivot`, `pan`, `spin`,
    `revert_rotation`, `hold_rotation`, `reset`.
  - `frame(dt, terrain)`: `UpdateBasis`, then `UpdateCoords`.
  - `view()`, `projection()` and `view_proj()` come from the lanes.
    `eye_distance()` is `CameraGetZoom`, and `zoom()` is the target zoom.
  - The yaw and distance API is gone. Its callers take `heading`, `zoom`
    and the focus.
  - `target_manual(x, y, z, heading, pitch, zoom)` is `TargetManual` at
    once. It is used by `MoveTo`, `SnapTo`, `SetZoom` and `RestoreSettings`
    (which then reverts).
  - `set_free` is `cam_Free`.
  - `set_eye_distance(d)` solves for the zoom that puts the eye `d` from the
    focus. The render tests' views use it.
  - `update(window)` gathers a `CameraInput` from GLFW, and `apply(input)`
    acts on it, so the mapping is unit-tested.
- **`Camera::update(window, dt)`** is the world view's `Frame` and
  `HandleEvent`:
  - keyboard pan and spin;
  - Space spin and its revert;
  - the middle-drag pan.
  - The wheel reaches `zoom()` from the scroll callback.
  - The mouse's actions happen only over the world view, not over UI
    controls. The keys act only when no control has the keyboard.
- **The renderer** draws with the camera's projection (its FOV, near and
  far).
  - The strategic icons' fade still compares the eye distance, as before.
  - The shadow box keeps sizing from the eye distance.
  - The playable rect comes from the sim when it has one.
- **Lua:**
  - `GetZoom` returns the target zoom;
  - `SetZoom` sets it at once (timed moves are M217g);
  - `GetMinZoom` is 5, `GetMaxZoom` the farthest;
  - `Reset` is `CameraReset`;
  - `SaveSettings` / `RestoreSettings` and `GetFocusPosition` read and set
    the lanes.
- **`--camera x,z,zoom`** (captures and goldens) takes a zoom. The goldens
  are re-recorded (they live outside the repository).
- **Sound:** `CameraDistance` is Moho's LOD metric at the focus, which is
  the zoom. `ZoomPercent` is the zoom over the farthest. `Angle` stays the
  pitch for now; Moho's is each cue's angle to the listener, which is
  audio's to port.
- **The render tests' `OffscreenShots`:**
  - holds a free camera at their old 50° pitch;
  - frames each shot by the eye's distance, as before;
  - keeps a held turn across `build_scene`'s reset.

## Tests

- **`test_camera` (unit):** the rules above, against the formulas, on a
  flat terrain:
  - the farthest zoom, and the reset;
  - a notch's zoom, and the log glide's step, clamp and settling;
  - pitch and FOV across the zoom;
  - eye distance and eye position for a heading;
  - clip planes and projection;
  - the target's clamp;
  - pan by a pixel (direction and scale);
  - spin, its clamp and its revert over frames;
  - the pivot's ground point kept while zooming out.
- **`--camera-test` (gate)** gains, on SCMP_009:
  - the reset's view;
  - a wheel zoom glides and tilts the view;
  - a middle-drag moves the ground under the cursor with it;
  - Space-spin down to the horizon, and the view reverts on release;
  - the keyboard pan's rate.
- **Mutation:** each tunable, the log interpolation, the glide, the eye
  distance, the clamps, the pan and spin axes and signs, and the revert.
