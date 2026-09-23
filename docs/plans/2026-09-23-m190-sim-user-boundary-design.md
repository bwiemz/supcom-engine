# M190: The sim/user boundary

**Goal:** the renderer draws the world from per-tick snapshots of the sim, interpolated between the last two ticks, instead of reading the live sim. Motion is smooth at any frame rate, and the renderer no longer depends on the sim's internals, which later allows it to run on its own thread.

## Where it stands (2026-09-23)

- **Sim to UI Lua is done.** `sync_beat` copies the sim's `Sync` table into the user state each beat, and `OnSync` reacts.
- **The renderer reads the live sim.** Every frame, eight sub-renderers walk `SimState`/`EntityRegistry`: `UnitRenderer`, `OverlayRenderer`, `ParticleSystem`, `StrategicIconRenderer`, `MinimapRenderer`, `HudRenderer`, `SelectionInfoRenderer` and `FogRenderer`.
  - They read about 40 distinct fields, including positions, orientations, bone matrices, health, command queues, intel states, economy and the fog grid.
- **There is no interpolation.** The sim ticks at 10 Hz, so units, projectiles, health bars and selection rings jump every 100 ms.
  - Skeletal poses are computed once per tick too (`Unit::tick_manipulators`), so walk cycles and turret motion also step at 10 Hz.
- **The render path writes to the sim.** `render()` clears the camera-shake queue and `OverlayRenderer` clears the death-event queue.
- **Picking uses tick positions.** `InputHandler` ranks units by their live tick positions, not by where they are drawn.
- **Snapping is ignored.**
  - `Entity:SetPosition(pos, immediate)` ignores `immediate`, and `Warp(entity, pos, orient)` ignores its orientation.
  - These are how FA scripts ask for a teleport, as opposed to motion the renderer should interpolate.

## How Moho does it

- Moho's user side holds each entity's previous and current tick transform. It draws at `prev + alpha * (cur - prev)`, where alpha is how far the wall clock has moved into the current tick at the current game speed.
- The world is therefore drawn up to one tick behind the sim, which is the usual cost of interpolation.
- An immediate `SetPosition`, or a `Warp`, makes the next transform replace both the previous and current one, so the entity jumps instead of sliding.

## Design

### Snapshots (`src/sim/world_snapshot.{hpp,cpp}`)

- **`WorldSnapshot`:** one tick of render state.
  - `EntityPose` records sorted by id: position, orientation, beam endpoint, snap serial, and an offset/count into a pooled bone-matrix array.
  - Ids are sequential and never reused within a session, so matching by id across two snapshots is safe.
- **`capture_world(const SimState&, WorldSnapshot&)`:**
  - fills a snapshot, reusing its capacity;
  - skips destroyed entities;
  - copies bone matrices only for units that have an animated pose.
- **`WorldHistory`:** the previous and current snapshots. `capture(sim)` swaps them and captures into the new current one. `clear()` runs on reload.
- **Hook:** `SimState::set_tick_observer(fn)` runs after every tick. The app points it at `history.capture` and `clock.on_tick`.
  - Single-player ticks, lockstep ticks (inside `LockstepSession::receive_and_advance`) and harness ticks all go through `SimState::tick`, so there is one hook and no per-caller bookkeeping.

### Interpolation (`FrameView`)

`FrameView(prev, cur, alpha)` answers `pose(id)`, `position(entity)`, `orientation(entity)`, `beam_end(entity)` and `bones(id, out)`.

| Case | Result |
|---|---|
| Entity in both snapshots | Position and beam endpoint lerped; orientation nlerped along the shortest arc; bone matrices lerped component-wise (as the animation cross-fade already does) |
| Entity only in `cur` (just spawned) | `cur` |
| Snap serial differs between the two snapshots (teleport) | `cur` |
| Attached, or the parent snapped | `follow_attachments` snaps a child on its first follow (attaching is a jump) and whenever its parent has snapped since, so a chain jumps link by link, each on the tick it moves |
| Entity in neither (spawned between ticks, e.g. by a SimCallback) | Live value, while the renderer still iterates the registry (M190a) |
| Default-constructed view (no history) | Live values, so callers without history keep working |

### Clock (`src/core/tick_clock.hpp`)

`TickClock` measures sim time since the newest tick.
- `advance(dt * speed)` runs each frame while the sim runs, not while it is paused or stopped.
- `on_tick()` subtracts one tick, clamped to `[0, tick]`.
- `alpha() = since / tick`, clamped to `[0, 1]`.

The loop's accumulator can't be used for alpha: in multiplayer it paces frame *sends*, and it keeps cycling while a lockstep stall holds the sim still, so units would jitter between the last two ticks. `TickClock` follows real tick arrivals:
- **Steady single-player:** it tracks the accumulator.
- **During a stall:** it holds at `cur`.
- **After a stall:** interpolation resumes within one tick.
- **While paused:** it stays frozen.

### Renderer changes

**M190a**
- `Renderer::render` takes a `FrameView`, and every per-frame position or orientation read goes through it:
  - unit, prop, projectile and wreckage models, and their bone poses;
  - health bars, selection rings and command lines;
  - intel rings, operation beams and collision beams;
  - shields and IEffect beams;
  - attached particle emitters;
  - strategic icons and the minimap.
- A mesh and its overlays therefore always agree.
- `InputHandler` picks against the same interpolated positions.
- `SetPosition(pos, true)` and `Warp` bump the entity's snap serial; `Warp` also applies its orientation.

**M190b** (done)
- The snapshot grows to cover everything the renderer reads:
  - per entity: kind, army, mesh key, scale, health, build fraction, the icon class precomputed from categories, operation state and target, veterancy, cargo, silo ammo;
  - per army: colours;
  - for the focus army: economy, a copy of the fog grid, and `player_result`;
  - a side table for the selected units' command queues and intel states;
  - the effect records.
- The death and camera-shake queues are moved into the snapshot at capture, so the renderer never writes to the sim.
- The renderer no longer reads the build ghost from `SimState`. Input (`InputHandler::build_ghost`) works out its snapped spot and validity from the sim, and hands the renderer a `BuildGhost`. The ghost's *storage* moves out of `SimState` with the UI bindings that set it (M191).
- `Renderer::render` and `build_scene` lose their `SimState&`.
- In `src/renderer`, only `InputHandler` includes a sim-state header. It is input, which works on the live sim and moves out of the renderer with M192. `mesh_cache`/`renderer.cpp` also include `sim/scm_parser.hpp`, which is the SCM file format, not sim state.
- The sim clears its death and shake queues at the end of each tick, after the capture has copied them. `WorldHistory` holds them until the renderer shows them. This also fixes headless runs, which never render, keeping every event forever.
- **Checked with `--render-dump`.** A scripted scene (tanks, bots, a shield, an engineer building, army 1 selected) is rendered offscreen, and everything the renderers generate is dumped for 21 frames. The dump after the refactor is byte-identical to the one taken before it.

**Later**
- The UI state's user-unit bindings (`UserUnit:GetPosition`, `GetHealth`…) read the snapshot too. That belongs with M191, which splits the bindings by Moho class into sim and user sides.

## Testing

**Unit (no data)**
- `TickClock`:
  - steady cadence equals the fixed-step accumulator;
  - a stall holds alpha at 1 and recovers within one tick;
  - paused frames freeze alpha.
- `capture_world`:
  - sorted ids;
  - destroyed entities skipped;
  - bone pool offsets;
  - capacity reused.
- `FrameView`:
  - the midpoint;
  - the quaternion shortest arc (q vs −q);
  - spawn, teleport and parent-teleport snaps;
  - bone lerp;
  - live fallback.
- Snap bindings: an immediate `SetPosition` and `Warp` bump the serial; a plain `SetPosition` doesn't.

**Data-backed: `--interp-test`**
- Runs the real windowed loop headless on a fixed clock at four frames per tick, orders army 1's ACU to move, and records the drawn ACU position every frame.
- The run passes when the position changes on nearly every frame while the unit is moving (without interpolation it changes on one frame in four), and every drawn position lies on the segment between the two ticks' positions.

**Goldens**
- Units are now drawn up to a tick behind. Any golden with moving units is re-captured deliberately, after the diff has been inspected.

## Milestones

| # | Scope |
|---|---|
| M190a | Snapshots, `TickClock`, `FrameView`; every renderer and picking reads interpolated poses; Moho's snap semantics; `--interp-test` |
| M190b | The renderer reads only snapshots: full per-entity records, per-army and focus-army state, event queues moved into the snapshot, the build ghost moved to user state |
