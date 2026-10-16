# The sim on its own thread (OE-134)

**Goal:** a sim tick no longer holds a frame back. Today each frame runs the
tick on the main thread, then draws; a 20 ms tick in a 16.7 ms frame is a
dropped frame ten times a second, and a 60-95 ms Lua collection is several.
Ryan, 2026-10-16: go ahead, "we want a playable game".

## The shape: a pipelined tick

M190 already drew the world from per-tick snapshots (`WorldHistory`,
`FrameView`), so the renderer reads no live sim. What remains on the main
thread that touches the live sim (the UI's Lua, input picking, orders, the
sim-to-UI `Sync`, sounds, telemetry) stays there, at one point in the frame
where no tick runs:

```
frame N:  [collect tick N-1's results] UI, input, orders, beats, sounds
          [prepare the draw: ghosts, overlays from the live sim]
          [start tick N on the sim thread] ---- tick N runs ----.
          draw (snapshots only), present                         |
frame N+1:[collect: wait for tick N if still running] <----------'
```

- **The collect** (`complete_sim`) waits for the tick in flight, then does
  what used to follow it in the same frame: publish its snapshot, the tick
  clock, telemetry, the UI's beats (`world_beat`), dropped peers, a network
  save's notice to the UI.
- **The start** (`start_sim`) decides whether a tick is due (the same
  accumulator and lockstep rounds) and hands it to the sim thread: one
  tick, or one lockstep round, per frame.
- **The draw** reads only snapshots and what the frame prepared before the
  start.

A tick shorter than about a frame and a half no longer drops a frame: it
runs while the previous frame is drawn and presented.

## What must change

1. **The snapshot is published, not captured, on the main thread.** The
   tick captures into a third buffer (`WorldHistory::capture_staged`); the
   collect rotates it in (`publish`): previous <- current <- staged. The
   events (deaths, shakes, sounds) are staged the same way. A caller with
   no thread (headless, tests) still calls `capture`, which does both.
2. **Nothing in the tick calls into the UI's Lua.** A network save (OE-128)
   is written on the sim thread right after its tick (the profile is known
   at attach), and its notice waits for the collect.
3. **Dropped peers** are handed back by the round and disconnected at the
   collect.
4. **The profiler is not thread-safe:** under `--profile` the tick runs on
   the main thread as before (and so do catching up a loaded game, which
   checks every tick, and `--no-sim-thread`).

## Why games stay the same

The tick is the same code on another thread, and every input to it (orders,
SimCallbacks, lockstep frames) is given between ticks, at the collect, in
the same order as before. A game played with and without the sim thread
must give the same telemetry record by record, and every peer the same
checksums: CI's matches and lobby games run with it on.

## Not in this step

- Running ahead: ticks are paced by frames (one per frame at most), so a
  frame rate under 10 slows the game as it does now.
- The Lua collection's pause (OE-135) still makes its tick long; it now
  costs a frame or two instead of six.

## As built (2026-10-16)

- `SimThread` (`src/app/sim_thread.hpp`): one job at a time; what a job
  throws is thrown at the collect.
- The draw also ran the UI's frame callbacks and events (in
  `Renderer::render`), and a callback reading a unit's command queue
  captured the live world (`ui_world`) mid-tick: a desync in LAN games,
  caught by a check on the sim's Lua state (`lua/lua_thread_check.hpp`,
  through the vendored Lua's `lua_lock`). The UI's frame now runs before
  the tick starts (`Renderer::run_ui_frame`), only on frames that are drawn
  (a fast match draws one in fifty, as before), and `ui_world` serves the
  published capture while a tick runs.
- A network save (OE-128) is written on the sim's thread with the profile
  taken when the game began; the UI is told between frames.
- Checked: fast matches with and without it play the same game (every
  telemetry record); LAN games, a player leaving, and a saved game resumed
  pass with it; five real-time minutes drop 44 frames against 220.
