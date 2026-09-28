# M217g: The camera's moves

## Why

M217f made the camera's basis Moho's, but every scripted move is still
instant:
- `MoveTo`, `SetZoom` and `MoveToRegion` ignore their seconds;
- `UIZoomTo` only recentres the camera;
- `TargetEntities`, `TrackEntities`, `NoseCam`, `Spin` and the clocks are
  stubs.

These moves are how FA's scripts talk to the camera:
- The sim's `SimCamera` queues camera requests through
  `Sync.CameraRequests`.
- `usercamera.lua` runs them on the UI's camera, then calls
  `WaitFor(camera)` until a move ends, then `SimCallback`, so the sim's
  script can go on.
- Campaign cinematics, `ScenarioFramework`'s death cameras and the
  idle-unit buttons all go through this.

In the engine the waits return at once, so a script's sequence of moves
collapses into one frame.

The rules come from faf-re's `CameraImpl`:
- `TimedMoveInit`, `SetupHermite`, `InterpolateBasis`, `UpdateTargets`;
- `TargetManual`, `TargetBox`, `TargetEntityBox`, `TargetEntities`,
  `TargetNextEntity`, `TargetNoseCam`, `TargetNothing`;
- `Frame`, `CameraSetAccType`, `GameTimeSource`, `SystemTimeSource`;
- the Lua methods.

`cfunc_UIZoomToL` supplies `UIZoomTo`'s box.

## The rules

### A timed move (`TimedMoveInit(seconds, transition)`)

- When `seconds > 0`, it records its start on the camera's clock: the
  focus, target zoom, pitch and heading (the heading wrapped to ±π). It
  also clears the camera's event, so `WaitFor(camera)` waits.
- The move's end is the target, `nearZoom`, the end pitch and the end
  heading.
  - The end heading is unwrapped to within π of the start
    (`NormalizeQuadrant`). (faf-re's `TargetManual` unwraps against the
    camera's raw heading instead. After a move that ended past ±π, the
    next move across ±π would then turn a full circle the long way round.
    Campaign markers sit near π, so that would show in FA's cinematics. The
    nose camera, in the same decompile, unwraps against the move's start,
    and so does the port.)
- **Ease (`SetupHermite`):**
  - Ease-in-out is on after a reset. The tangents are then zero, so the
    blend is the cubic `3t² − 2t³`.
  - With ease-in-out off (`DisableEaseInOut`), both tangents are the whole
    change, which blends linearly.

### Each frame (`Frame`)

1. **The clock:**
   - On the game clock (`UseGameClock`), `dt` is the game time's change,
     where game time is `(tick + interpolant) × 0.1`.
   - Otherwise (the system clock, the default) `dt` is the frame's.
2. **`UpdateTargets`:**
   - **An entity or nose target:** the target follows the entity's
     interpolated position.
     - A nose target also takes the entity's heading, and its pitch plus
       the adjustment.
     - When the entity is gone, the target becomes a location, and a
       rotated view is reverted.
   - **A spin (`Spin(headingRate, zoomRate)`):** the end heading grows by
     `rate × dt × 2π` and the zoom asked for by `zoomRate × dt`.
3. **During a move, `InterpolateBasis`; otherwise `UpdateBasis`**
   (M217f). `InterpolateBasis`:
   - `progress = (now − start) / seconds`.
   - At or past 1:
     - the focus is the target, and the target zoom the zoom asked for;
     - the move ends and the camera's event is signalled;
     - a Hermite or nose move holds the end heading and pitch, rotated.
       (faf-re's decompile reads the *start* pitch here, which would snap
       every such move back where it began; its `UpdateBasis` goes on to
       hold the end pitch, so the port holds the end pitch.)
   - Otherwise:
     - The progress is shaped by the acceleration: `Linear` as it is,
       `FastInSlowOut` as `sin(p·π/2)`, `SlowInOut` as the two cosine
       halves. A nose move shapes by its transition's own seconds.
     - The zoom and focus are blended, with the Hermite weights of the
       shaped progress, from the start to the end.
     - A Hermite or nose move blends the heading and pitch too, rotated.
   - A move that isn't targeted (a location or box) takes its pitch from
     the zoom's log, as the basis does.
   - The FOV follows the zoom.
   - The focus drops onto the ground along the view.
   - Once finished, Hermite and box moves become plain locations.

### The targets

- **`TargetManual(pos, heading, pitch, zoom, seconds)`**: with `seconds`,
  a Hermite move to them (M217f made the 0-second form).
- **`TargetBox(box, seconds)`**: the box's centre, zoomed to its wider X or
  Z extent.
  - At once: clamped, focused, with its FOV.
  - With seconds: a box move.
- **`TargetEntities(entities, track, zoom, seconds)`**: the first live
  entity, at `zoom`.
  - Tracked, it is an entity target, followed each frame.
  - Otherwise, a location.
- **`TargetNoseCam(entities, pitchAdjust, zoom, seconds, transition)`**:
  a nose target on the first entity, at its heading and pitch plus the
  adjustment. The view is set at once, then eased from the start.
- **`TargetNothing`**: a location target.

### Lua (the camera object)

| Method | Does |
|---|---|
| `MoveTo(pos, hpr, zoom, seconds)` | `TargetManual` |
| `SnapTo(pos, hpr, zoom)` | `TargetManual` at once |
| `SetZoom(zoom, seconds)` | `TargetManual` to the target, heading and pitch |
| `MoveToRegion(rect, seconds)` | `TargetBox`, the rect's x/z at the ground's height |
| `TargetEntities(ids, zoom, seconds)` | `TargetEntities`, untracked |
| `TrackEntities(ids, zoom, seconds)` | `TargetEntities`, tracked |
| `NoseCam(id, pitchAdjust, zoom, seconds, transition)` | `TargetNoseCam` |
| `Spin(headingRate, [zoomRate])` | set the rates, rotated, as a Hermite target |
| `SetAccMode(name)` | `Linear`, `FastInSlowOut` or `SlowInOut` (case-insensitive) |
| `EnableEaseInOut` / `DisableEaseInOut` | the ease flag |
| `HoldRotation` / `RevertRotation` | M217f's |
| `UseGameClock` / `UseSystemClock` | the move's clock |

- The entities are given as tables of entity ids (strings, as retail
  passes them).
- `WaitFor(camera)` in a UI thread parks the thread until the camera's
  event is signalled (a move ends). With no move under way it returns at
  once.
- **`UIZoomTo(units, [seconds])`:**
  - The units' box, 20 wider each way (the height being their mean ±
    half the wider extent, and 20).
  - Then `TargetBox`, then `TargetNothing`.
  - `UISelectAndZoomTo` selects the units, then zooms the same way.

## The engine

- **`renderer::Camera`** gains:
  - the target type, the move's start and ease;
  - the acceleration, the clock, the spin rates;
  - the target entities, and a signalled flag with its waiters.
- **The renderer supplies each frame:**
  - the clocks: the system's seconds, and the game's
    `(tick + interpolant) × 0.1`;
  - a lookup of an entity's interpolated position and orientation, from
    the frame's view.
- **`WaitFor`:** the UI thread's `WaitFor` takes a camera object. It parks
  on a waitable, which the camera wakes when its event is signalled.

### The sim's side: suspended threads and the callback

The sim script waits on a `SimCamera`, a `SingleEvent`:
`OnEvent(ResumeThread, CurrentThread())`, then `SuspendCurrentThread()`.
The UI's `SimCallback({Func = 'OnCameraFinish', Args = name})` sets the
event later, and that calls `ResumeThread`. The engine had stubbed all
three: `SuspendCurrentThread` was `WaitTicks(1)`, and `ResumeThread` did
nothing. So a script's camera waits ended after one tick, whatever the
camera did. Now:
- `CurrentThread()` returns the running thread's handle, the one
  `ForkThread` returned (nil outside a thread);
- `SuspendCurrentThread()` puts it to sleep with no tick to wake at;
- `ResumeThread(handle)` wakes it for the next pass, when it is suspended
  (a thread sleeping on `WaitTicks`, or a stale handle, is left alone).

`SimCallback` read `Args` only when it was a table. SimCamera's is the
camera's name, so the sim's `OnCameraFinish` got nil. A callback now
carries `Args` as one value (a string, number or bool) when it is one.
Callbacks are commands, recorded in replays and sent between peers, so the
command codec writes the value after the callback's units. Replays move to
version 8; older ones are read without it.

Who answers a wait:
- **A game with a UI:** usercamera.lua does, once the move ends.
- **A UI with no renderer:** the camera's methods do nothing and
  `WaitFor(camera)` returns at once, so the script goes on in the next
  beat. SimCamera's comment asks for that ("considered to finish
  immediately ... in a headless mode").
- **A replay:** the UI's `OnCameraFinish` is a recorded command, so a
  replay resumes the script on the tick the game did, UI or not.
- **Multiplayer:** each peer's UI calls back, and the first to arrive sets
  the event. SimCamera's comment says these waits aren't for multiplayer.

## Tests

- **Unit (`test_camera_moves`):**
  - a timed `TargetManual`'s blend at 0, ¼, ½ and 1 of its seconds, with
    ease-in-out and without, under each acceleration;
  - the end state, the event's signal, and the move becoming a location;
  - the game clock;
  - `TargetBox`'s centre and zoom;
  - an entity target following its entity, and letting go when it is gone;
  - the nose camera;
  - `Spin`'s rates.
- **Unit (`test_thread_wait`, `test_sim_callbacks`):** a suspended thread
  sleeps until `ResumeThread`, which leaves sleeping threads and stale
  handles be; a callback's one-value `Args` reaches `DoCallback`, is
  recorded and replayed, survives the codec, and is absent from a v7
  command.
- **`--camera-moves-test` (gate), on SCMP_009, through Lua:**
  - the game's opening: gamemain's `UIZoomTo(the ACU, 1)` 1.5 seconds in
    glides to it over a second;
  - `MoveTo` over 1 second is mid-way at 0.5 and there at 1;
  - `WaitFor(camera)` in a UI thread resumes when the move ends, and not
    before;
  - `UIZoomTo` frames the ACU;
  - `TrackEntities` follows a moving unit;
  - a sim script's `SimCamera:MoveTo` reaches the camera through
    `Sync.CameraRequests`, and the script resumes only after the move
    ends.
- **Mutation:**
  - the Hermite weights, the acceleration curves and the ease flag;
  - the progress, the end snap and the signal;
  - the box zoom, the tracking and the clocks.
