# M199 — Replays that work

Status: design, 2026-09-23. Builds on M195–M198 (ordered iteration, one sim
RNG, floating-point policy, all mutation in-tick).

## Why

M195–M198 make the engine deterministic: the same inputs produce the same
game on every peer and every platform. Replays put that property to use:

- A player can watch a game again, or share it.
- A bug report can carry the exact game that went wrong.
- CI can play a recorded game and check the checksum at every tick.
- A Windows build and a Linux build can be compared on real game data (the
  "cross-OS lockstep test" in the roadmap).

## What exists

- `sim::Replay` (format v3): seed, command delay, victory condition and a
  command list. It round-trips, and a unit test replays it into a fresh
  `SimState` and matches the checksum.
- Nothing in the running game records or plays a replay. `set_recording`
  has no production caller, and `SessionIsReplay` always returns false.
- Recording happens when a command is *scheduled*. Multiplayer never reaches
  that point, because `LockstepSession` submits straight to the scheduler, so
  an MP game would record nothing. A command scheduled after the game ends
  would be recorded even though it never ran.
- The header can't rebuild the game. It has no scenario, no armies (faction,
  team, start spot, AI or human, colour) and no options.
- Three launch paths build the game's setup in three different ways:
  - the CLI `--map` modes: armies from `--ai-skirmish`/`--ai-armies`, every
    army faction 1, default options;
  - the lobby reload: `sessionConfig` is parsed into `ArmySlotConfig` and
    `GameOptionsConfig` inside `execute_reload_sequence`;
  - LAN: a fixed 1v1 on SCMP_009.

## Design

### 1. One launch description: `GameSetup`

`GameSetup` is plain data in `src/sim/game_setup.hpp`: the sim layer, so
`Replay` can hold it without depending on Lua. It holds:

- the scenario path and the seed;
- the army slots, one `ArmySlot` per army, in scenario order: human, or AI
  with a personality; faction; team; start spot; player and army colour;
  handicap;
- the number of armies to create;
- the game options, typed (`GameOptionValue`) and in the lobby's order,
  plus restricted categories.

`ArmySlotConfig` and `GameOptionsConfig` move here from `session_manager.hpp`.
The `osc::lua` names become aliases, so `SessionManager` keeps its API.

Every path turns its inputs into a `GameSetup`, then launches through one
function:

- **CLI:** the flags. `--ai-skirmish`, `--ai-armies` and `--ai-personality`
  keep their meaning. New flags: `--faction N` (all armies) and
  `--option Key=Value` (repeatable).
- **Lobby:** `sessionConfig` → `GameSetup`. The parsing moves out of
  `execute_reload_sequence`.
- **LAN:** the host's `GameSetup`. Today that is still the fixed 1v1; sending
  the full setup from the lobby is Phase G.
- **Playback:** the replay's header.

The replay header is then exactly the object that launched the game, and
playback cannot drift from recording.

### 2. Record what the sim applies, where it applies it

`SimState::dispatch_due_commands` appends each command it applies,
callbacks included, to the recording, with the tick it ran on. This one
place captures:

- single-player and multiplayer alike: local, remote and callback commands
  all dispatch here;
- only what ran, in the order it ran (the scheduler's canonical order).

Playback submits the recorded commands in that order. The scheduler's sort
is stable and keyed on (source, sequence), and sequence follows submission
order, so it reproduces the same order.

Events that change the game outside the command stream become commands on
the stream:

- A dropped peer's defeat (`defeat_army` from the game loop) becomes an
  engine callback scheduled for the next tick. It still isn't a consensus
  decision (that is M198b), but it happens inside a tick, and a replay
  reproduces what this peer did.

### 3. The replay file (format v4)

| Field | |
|---|---|
| magic, version | `OSCR`, 4 |
| engine build | `git describe` string. Informational; playback warns on a mismatch |
| `GameSetup` | as above |
| command delay | |
| commands | as dispatched: exec tick, source, command or callback, unit ids |
| checksum trail | the sync checksum after every tick (4 bytes a tick, about 140 KB an hour) |
| final tick | |

Older versions (1–3) still load, but they carry no setup, so they can't be
played as a game. Their unit-test use stays.

### 4. Recording in play

- Every game records. The file is written when the game ends, and on exit,
  to `<user data>/replays/LastGame.oscreplay`.
- `--record <file>` names the file (tests and captures).

### 5. Playback

`--replay <file>` does the following:

1. Loads the header and launches its `GameSetup`.
2. Queues the command stream.
3. Runs to the final tick.

The sim takes no player input during playback: a human-routed order is
dropped with a log line.

- **Headless** (`--replay` with no window, or with `--ticks`): it compares
  each tick's checksum with the trail and reports the first mismatch (tick,
  and the parts via `--checksum-trace`), exiting non-zero. This is the
  regression tool.
- **Windowed** (M199b): the observer view, `SessionIsReplay()` true (retail's
  UI hides orders in a replay), and game speed controls.

## Tests

Unit tests:

- the `GameSetup` round trip, and a v3 file still loading;
- recording at dispatch captures a lockstep peer's remote commands and
  callbacks;
- playback into a fresh sim matches the trail;
- a player's order during playback is dropped.

`data.replay_roundtrip` (gate), on retail data:

1. Process A plays SCMP_009 with four armies. Armies 2–4 are AI; army 1 is a
   "player" whose scripted orders go through the human route
   (`--scripted-orders`): moves, a factory queue, a pause, a fire state. A's
   stream therefore holds orders, unit settings and callbacks.
2. A records the game.
3. Process B plays the replay. Every tick's checksum must match, and so must
   the final one.

There will be no golden replay in the repo. A recorded game's outcome changes
whenever a game rule changes, so a committed golden would break on every
gameplay fix. The round trip checks what matters: every input is captured.
Golden replays of real games can live outside the repo, in `OSC_GOLDEN_DIR`,
as tracking artifacts.

## Cross-OS (M199c)

- The CI Windows job also builds Release and uploads `opensupcom.exe` and its
  DLLs as an artifact. Release is needed because Wine lacks the debug CRT.
- `tools/cross_os_replay.py` does the following:
  1. fetches that artifact for a commit (`gh run download`);
  2. records a replay with the Linux build;
  3. plays it under Wine with `--fa-path` pointing at the same retail data;
  4. diffs the two checksum traces with `tools/checksum_diff.py`.
- It needs FA data, so it runs locally, not on CI. It is documented in
  CONTRIBUTING and run before each merge that touches the sim's arithmetic.

## Slices

- **M199a:**
  - `GameSetup` and one launch function for the CLI and lobby paths;
  - recording at dispatch;
  - peer-drop defeat as a stream event;
  - format v4 with a checksum trail;
  - headless `--record`/`--replay` with divergence reporting;
  - `--scripted-orders`;
  - `data.replay_roundtrip`.
- **M199b:** windowed playback (the observer UI, `SessionIsReplay`,
  `LastGame.oscreplay`, speed controls, retail's replay UI hooks).
- **M199c:** the cross-OS check through Wine.

## Out of scope

- Reading or playing FA's own `.SCFAReplay` files. They need FA's command
  encoding, and they would only reproduce a retail game if the sim matched
  Moho bit for bit.
- Save games (M208).
