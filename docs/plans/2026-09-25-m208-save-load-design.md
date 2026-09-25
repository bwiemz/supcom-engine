# M208 — Save and load

Status: M208a done, M208b measured (2026-09-25); M208c (snapshots) needed for long games. Phase E (gameplay fidelity).

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

- **Saving.** `InternalSaveGame(path, name, callback)`, on the UI state, writes the game's recording so far as a save file (`sim::SavedGame`):
  - The file has a small header (`OSCSAVE`, a version, the build, the name, the tick), then the replay's own serialization (`Replay::serialize`, in its current format). The scenario is in the replay's setup.
  - The recording already holds the setup, the commands, the build, and the checksum trail up to the current tick.
  - **Orders still to run are saved too.** The UI runs between ticks, so a click just before saving, or any order given while paused, is in the scheduler and not yet in the recording. The save appends them, and they run after the load, on their own ticks.
  - It is written to a temporary file, then renamed over the old one, as Moho does, so a failed save never destroys the file it would replace.
  - It is written only into the SaveGame folder, since the path comes from scripts.
  - The callback gets `true`, or `false` and a reason: no game running, a multiplayer game, a replay, a game still catching up, a path outside the folder, or a file that can't be written (`'nowrite'`, which retail's dialog words for itself).
  - Moho calls back once its sim reaches the end of a tick. The engine's UI already runs between ticks, so it saves and calls back at once.
- **Loading.** `LoadSavedGame(fspec)` reads the header:
  - an unreadable file is `CantOpen`, and a bad header `InvalidFormat`;
  - a save from another build is `WrongVersion`, as retail refuses a save from another version.

  It then launches the game as `LaunchReplaySession` does, through the reload sequence, but in a new mode, **resume** (`SimState::start_resume`):
  1. **Catch up.** The sim plays the recorded commands as a replay plays them, with the player's input dropped. Each tick's checksum is checked against the trail. It runs in the game's frame loop, as many ticks as fit in 100 ms each frame: the window stays live, shows the game fast-forwarding, and its title says `LOADING`. The load is asynchronous, as retail's launch is, so `LoadSavedGame` has already returned by now. A difference therefore sends the player back to the front end, with a notice that the save did not load as it was played.
  2. **At the saved tick,** the sim itself ends playback, at the end of that tick, so every tick driver gets the same handover. The sim takes the player's orders again. The recording started with the load and has grown back to the whole game, so a later save holds all of it.
- **`SessionIsReplay()` is false throughout.** Retail's UI asks it while building the game interface (menus, score, unit view), so a loaded game must never look like a replay, even while catching up.
- **The UI during catch-up.** Retail's UI learns about the sim through `Sync`, some of which is events. Catch-up therefore runs the sim-to-UI sync on every tick, as a played game does. M208b measures what this costs.
- **Headless:** `--load <file>` with `--ticks` or `--ai-skirmish` catches a save up (exit 1 on a divergence) and plays on. `--save <file> --save-at <tick>` saves a game mid-run. Without them, `--load` opens the save in the window, as the Load dialog does.
- **Not saved:** the camera, the selection, control groups, chat, and pause state. The game loads unpaused, with the camera on the player's start. These are presentation, and can come later.
- **Multiplayer:** no. `InternalSaveGame` refuses while a lockstep session is active, as the UI already stops it.

## M208b — measured (2026-09-25)

Four retail AIs on SCMP_009, seed 4242, a Release build of M208a. Each game was saved after its last tick and then loaded headless. The load time is the catch-up, from the load's own log.

| Game | Played in | Loaded in | Save |
|---|---|---|---|
| 10 minutes (6,000 ticks) | 20 s | 11 s | 24 KB |
| 30 minutes (18,000 ticks) | 6 min 9 s | 6 min 3 s | 72 KB |
| 60 minutes (36,000 ticks) | 37 min | 41 min | 144 KB |

(The 60-minute game shared the machine with other runs.)

**A load costs what the game cost to play.** A headless catch-up is the sim and nothing else, so the savings this section once proposed (sounds, effects, the world snapshot) don't exist there. In the window, catch-up adds only the per-tick sync to the UI. The design's estimate, "a 30-minute game loads in about a minute", assumed the early game's 285 ticks a second. Late four-AI games are far slower: 1,700 to 2,400 units, and 50 to 100 ms a tick.

**The response is the sim's speed first** (Phase H, M224), because it speeds up play as well as loading. A late-game profile found that a third of the sim went to category matching and a quarter to radius queries. M224a–c (#90–#92) cut the 18,000-tick sim from 450 s to 250 s, with identical traces, and loads speed up the same.

**Long games still need M208c.** Even at twice the speed, a 60-minute four-AI game would take about 20 minutes to load, and retail loads one at once. State snapshots are therefore the plan for long games. They are a milestone of their own, with a design first. Until then, loads suit short and medium games, and the sim-speed work continues.

## M208c — state snapshots (only if M208b says so)

- A snapshot of the sim's state at the saved tick, saved beside its history. A load restores the snapshot, then checks it: it plays a few recorded ticks and compares checksums, and on any doubt it falls back to catching up.
- **What it needs:**
  - Lua persistence (Pluto, or a hand-written walker of the sim's Lua state), with `_c_object` pointers written as entity ids and restored;
  - a serializer for each sim system, each with a round-trip test (save, load, compare every checksum domain);
  - coroutines: the sim's script threads, with their stacks.
- **This is a milestone of its own,** with its own design, once M208a works and M208b has numbers.

## Proof (M208a)

- **Unit (`[savegame]`):**
  - the save round-trips;
  - an empty, truncated, foreign, other-format or other-build file is refused with the right error;
  - a loaded game plays on as the saved one did. It drops the player's input while catching up, runs the order that was pending at the save, takes orders again, and its recording regrows to the whole game;
  - a game saved before its first tick is the player's at once.
- **Data-backed (retail), `data.save_load`:** three processes, on a four-AI SCMP_009 game with scripted player orders:
  1. A plays 1,200 ticks and saves after tick 600.
  2. B loads A's save, catches up (checked against A's checksums), plays on, and saves after tick 900.
  3. C loads B's save and plays to 1,200.

  Each save is made with one of the player's orders still pending. B's trace must match A's to tick 900, and C's must match B's throughout, domain by domain.
- **The UI flow, `data.load_flow`:** offscreen, through the globals retail's dialogs call.
  - `GetSpecialFiles('SaveGame')` lists the save, a missing save is `CantOpen`, and `LoadSavedGame` loads the listed one.
  - The game catches up with `SessionIsReplay()` false, and plays on.
  - `InternalSaveGame` saves it again: the new save is listed and holds the whole game.
  - A path outside the folder is refused, and nothing is written.
- **Mutation-checked:** each test above fails without the rule it guards. The rules: pending orders saved, the handover, `SessionIsReplay()` during catch-up, and the folder check.
- **Not covered:** the refusals for a replay and a multiplayer game (simple checks, with no test).

## Found along the way: starting a game from inside a game

Loading from the game menu's Load dialog starts a new game from inside the one being played. That path has problems of its own that have nothing to do with saves, and they affect any launch from a running game (and lobby, game, lobby, game):
- **Retail expects a fresh UI Lua state for every game.** Its `FrontEndData` exists to carry data across that reset. The engine keeps one UI state. So on the second game, `unitviewDetail`, which sets its module global `View` to nil and then reads it, errors, and the unit view is broken.
- **A GPU allocation leaks** on each relaunch (VMA asserts at exit in Debug builds).
- **Per-map textures are cached by name** (`__terrain_blend0/1`, `__normal_overlay__`) and not cleared with the scene. A second game on a different map would draw the first map's.

M208a fixes one prerequisite: the Load and replay dialogs destroy `GetFrame(0)` as they leave, and the root frame now keeps itself while its children go. The rest is a follow-up milestone of its own. Loading from the front end (the main menu, the single-player lobby) works.

## Risks

- **A UI callback given in the same frame as the save** (`SimCallback`) is queued for the next frame's submission, so it can miss the save. Orders go straight to the scheduler, and are saved.
- **Determinism holes.** Anything that affects the sim but isn't recorded breaks loads. Examples: UI state read by the sim, wall-clock time, or a SimCallback outside the command stream. The checksum trail catches this at load, and the save-load test is designed to hit it. It is the same property that multiplayer and replays already depend on.
- **Build changes.** A save from an older build may not replay identically, so saves are refused across builds, as retail does. This is stricter than it needs to be for builds that only touch the renderer, but it is safe.
- **Long games** load slowly until M208b or M208c.
