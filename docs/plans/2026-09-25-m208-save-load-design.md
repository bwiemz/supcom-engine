# M208 — Save and load

Status: design, 2026-09-25. Phase E (gameplay fidelity).

## Why

Forged Alliance saves a single-player game and loads it later: from the game menu, from quick-save, and from the front end's Load dialog. The engine can do neither. Retail's UI already calls the engine for both, so the dialogs open and then fail.

## What retail's UI asks of the engine

| Call | Where | What it expects |
|---|---|---|
| `InternalSaveGame(path, name, callback)` | `ui/dialogs/saveload.lua` (the Save dialog), `ui/game/gamemain.lua` (quick-save) | The game saved to `path`. Then `callback(worked, errmsg)`. |
| `LoadSavedGame(fspec)` | `ui/dialogs/saveload.lua` (the Load dialog) | Returns `worked, error, detail`. `error` is `'WrongVersion'`, `'CantOpen'`, `'InvalidFormat'` or `'InternalError'`. On success, the game starts. |
| `GetSpecialFiles('SaveGame')`, `GetSpecialFileInfo`, `RemoveSpecialFile`, `GetSpecialFilePath` | the dialogs | Already bound (M199). The type is `SaveGame` (`.oscsave`, in `savegames/`). |

Saving is offered only in single-player: the UI checks `SessionIsMultiplayer()` and `SessionIsReplay()`.

## Two ways to save a game

1. **The sim's state.** This is what Moho does, and it is fast to load. Save every C++ object (entities, brains, economy, navigators, weapons, manipulators, influence maps, the scheduler and more) and the sim's whole Lua state: globals, closures, and coroutines with their stacks. Lua 5.0 can be persisted with Pluto. But the engine's Lua objects point at C++ objects by address (`_c_object`), which would have to be rewritten as ids. Every system that holds sim state would need a serializer, and a missed field is a silent desync after load.
2. **The game's history.** The engine records every game: its setup (seed, armies, options) and every command, with the tick it ran on. Since M199 that recording plays back identically, checked tick by tick against its checksum trail, even across Windows and Linux. A save can be that recording. Loading plays it back as fast as the sim will run, then hands the game to the player at the tick it was saved.

**The decision: history first (M208a).** It is correct by construction; the determinism M196–M199 proved is what it rests on. It needs no serializer per system, and it survives every later change to the sim that keeps replays working.

Its cost is load time, which grows with the game. On SCMP_009 with four AIs, the Release build runs about 285 ticks a second, so a 30-minute game (18,000 ticks) loads in about a minute. M208b will measure it on real games. If it is too slow, M208c adds state snapshots (way 1) on top, with history as the fallback and the test oracle.

## M208a — saves as history

- **Saving.** `InternalSaveGame(path, name, callback)`, on the UI state, writes the game's recording so far as a save file:
  - The file has a small header (`OSCSAVE`, a version, the name, the tick, the scenario), then the replay's own serialization (`Replay::serialize`, in its current format).
  - The recording already holds the setup, the commands, the build, and the checksum trail up to the current tick.
  - The callback gets `true`, or `false` and a reason: no game running, a multiplayer game, a replay, or a file that can't be written.
- **Loading.** `LoadSavedGame(fspec)` reads the header:
  - an unreadable file is `CantOpen`, and a bad header `InvalidFormat`;
  - a save from another build is `WrongVersion`, as retail refuses a save from another version.

  It then launches the game as `LaunchReplaySession` does, through the reload sequence, but in a new mode, **resume**:
  1. **Catch up.** The sim plays the recorded commands, as a replay plays them, with no rendering and without the UI's per-frame work, as fast as it runs. The loading screen shows the ticks done. Each tick's checksum is checked against the trail. A difference is an error: the dialog reports `InternalError` with the tick, and the player goes back to the front end.
  2. **At the saved tick,** playback stops. The sim takes the player's orders again, and `SessionIsReplay()` is false. The recording continues from the loaded one, so a later save holds the whole game.
- **The UI during catch-up.** Retail's UI learns about the sim through `Sync`, some of which is events. Catch-up therefore runs the sim-to-UI sync on every tick, as a played game does, and skips only the frames: rendering, UI frames and input. M208b measures what this costs.
- **Not saved:** the camera, the selection, control groups, chat, and pause state. The game loads unpaused, with the camera on the player's start. These are presentation, and can come later.
- **Multiplayer:** no. `InternalSaveGame` refuses while a lockstep session is active, as the UI already stops it.

## M208b — measure and speed up

- Time a load for 10, 30 and 60-minute four-AI games, in Release and Debug.
- Find where catch-up spends its time; `tools/` profiling (eu-stack) found Lua GC to be 37% of a tick.
- Cut what catch-up doesn't need. Candidates include sounds (already sim-clocked when headless), effects the renderer would draw, and the world snapshot capture.

## M208c — state snapshots (only if M208b says so)

- A snapshot of the sim's state at the saved tick, saved beside its history. A load restores the snapshot, then checks it: it plays a few recorded ticks and compares checksums, and on any doubt it falls back to catching up.
- **What it needs:**
  - Lua persistence (Pluto, or a hand-written walker of the sim's Lua state), with `_c_object` pointers written as entity ids and restored;
  - a serializer for each sim system, each with a round-trip test (save, load, compare every checksum domain);
  - coroutines: the sim's script threads, with their stacks.
- **This is a milestone of its own,** with its own design, once M208a works and M208b has numbers.

## Proof (M208a)

- **Unit:**
  - the save header round-trips;
  - a truncated, foreign or other-build file is refused with the right error.
- **Data-backed (retail), `data.save-load-test`:**
  1. Play a four-AI game on SCMP_009 for 600 ticks, with scripted player orders (`--scripted-orders`), and save it.
  2. Load the save into a fresh sim.
  3. Play both on for another 600 ticks.

  The two checksum traces must be identical at every tick, domain by domain. A save made in a game that was itself loaded must load too.
- **The UI flow:** from the Load dialog, as `--replay-flow-test` drives the replay dialog. The save the test wrote is listed, loads, and the game resumes at its tick with `SessionIsReplay()` false.
- **Refusals:** `InternalSaveGame` in a replay or a multiplayer session fails with a reason, and writes nothing.

## Risks

- **Determinism holes.** Anything that affects the sim but isn't recorded breaks loads. Examples: UI state read by the sim, wall-clock time, or a SimCallback outside the command stream. The checksum trail catches this at load, and the save-load test is designed to hit it. It is the same property that multiplayer and replays already depend on.
- **Build changes.** A save from an older build may not replay identically, so saves are refused across builds, as retail does. This is stricter than it needs to be for builds that only touch the renderer, but it is safe.
- **Long games** load slowly until M208b or M208c.
