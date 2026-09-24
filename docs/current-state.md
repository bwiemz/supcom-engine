# OpenSupCom Current State

Last reviewed: 2026-09-23 (retail AI, binding coverage; see `docs/ROADMAP.md` for the plan)

## What This Codebase Is

OpenSupCom is a C++20/CMake reimplementation of the Supreme Commander: Forged Alliance Moho engine. The active engine surface is split across:

- `src/lua`: Lua 5.0 integration, FA/FAF script bootstrap, moho class bindings, sim/UI globals, smoke harness.
- `src/sim`: simulation state, entities, units, weapons, economy, intel, orders, AI brains, platoons, manipulators, path interaction.
- `src/map`: scenario and `.scmap` loading, heightmaps, pathfinding grid, visibility grid, terrain quadtree.
- `src/renderer`: Vulkan terrain/unit/UI/HUD rendering, mesh/texture/shader caches, overlays, particles, water, minimap, strategic icons.
- `src/ui`: MAUI-style control tree, input dispatch, WLD UI provider, keymap, font metrics.
- `src/audio` and `src/video`: XWB/XSB/miniaudio audio and MPEG movie playback support.
- `tests`: Catch2 unit coverage for core systems plus focused renderer/sim/parser behaviors.

The code runs against real FA/FAF data via the VFS and currently boots Seton's Clutch far enough to load blueprints, parse the map, start FA AI code, spawn armies, build structures, and execute sim ticks.

## Platforms, Data Targets and Measured Status (2026-09-23)

| | Status |
|---|---|
| Linux | GCC 16 and Clang 22, Ninja + vcpkg presets `linux-debug` / `linux-release` / `linux-asan`. Warning-clean with `-Wall -Wextra`: CI's Linux jobs (GCC 14, Clang 18) build first-party code with `-Werror` (`OSC_WERROR`). |
| Windows | MSVC presets unchanged. CI builds and tests them; not re-verified by hand since the Linux work. |
| Retail FA 3599 (Steam) | Found automatically through the Steam libraries. Boots via retail `bin/SupComDataPath.lua`: glob mounts, `/schook` hooks, LuaPlus `#` comments and size hints, and the plain `Categories` lists. Headless SCMP_009 runs 100 ticks with 0 Lua errors. Units run their own retail script classes. 4 retail AIs play 10 game-minutes with 0 Lua errors and about 100 units, fighting (`--ai-skirmish --ai-armies 4 --ticks 6000`). An ASan build of the same run is clean. Retail's own front end boots and reaches a hosted skirmish lobby. In a windowed game, retail's own game interface runs and draws: economy, score, avatars, unit view, orders and construction panels, command-mode clicks and the minimap window. The C++ HUD placeholders remain behind `--legacy-hud`. Preferences are retail's Lua `Game.prefs` (profiles, options, window positions) in `<config>/opensupcom/`; tests and captures keep them in memory. Audio plays FA's own XACT data through an app-owned cue engine: interface sounds, retail's music thread, unit and weapon sounds with FA's categories, falloff curves and limits. It has been checked headless only; a listening pass is still to do. The world is drawn between the sim's last two ticks (M190a): the sim still ticks at 10 Hz, but units, walk cycles, projectiles and overlays move every frame. |
| FAForever data | Still supported through `--init`/`--faf-data` or `~/.faforever`. Not re-verified: this machine has no FAF install. |

| Metric | Value |
|---|---|
| Unit tests (Catch2) | 380 cases / 37,983 assertions (the RNG tests draw many values). Clean on GCC and under ASan+UBSan+LSan (Clang not re-run since M186). |
| Two-process MP tests (`ctest -L mp`, data-free) | 5/5 |
| Static analysis (`ctest -L lint`, LLVM 22) | clang-tidy ratchet at its baseline of 33 triaged findings. Changed lines follow `.clang-format`. |
| Data-backed gate on retail (`ctest -L gate`) | All 108 pass: 104 data modes (including the no-map lobby flow, `--gameui-test`, `--victory-test` and the offscreen `--interp-test`), `data.determinism` (two processes play a four-AI game identically), the `data.binding_coverage` ratchet, and two golden captures of FA's game interface at frame 600 (0.1% tolerance): the default profile, and one that shows the minimap window. |
| Data-backed modes failing on retail (`-L retail-gap`) | None. The last six closed with engine fixes: blueprints are read from the store, not FAF's `self.Blueprint`; `GiveStorage` persists; finished or paused animations hold their pose; `EnableIntel` ignores intel a unit lacks (retail `SetupIntel` had been cloaking every unit); `CanBuild` reads category names. Tests that assumed FAF-only script fields were also fixed. |
| Retail-only engine API still unbound | 54 globals and 30 methods (`opensupcom --binding-coverage`, ratcheted by `tests/integration/binding_baseline_retail.txt`). Many are UI-only. |

## Verified Locally

- `build/tests/Debug/osc_tests.exe` passes: 208 test cases, 5,026 assertions.
- `build/Debug/opensupcom.exe --help` runs and lists the current CLI surface.
- **Multiplayer (LAN lockstep), verified across two OS processes over localhost TCP:**
  `opensupcom.exe --mp-host` + `opensupcom.exe --mp-join 127.0.0.1` reach identical
  sync checksums with `desynced=0` through scripted player orders (incl. a mid-move
  Stop); adding `--mp-desync` makes both peers correctly report `desynced=1` for an
  injected divergence. Command routing (`SimState::route_command`) sends local human
  orders to the `LockstepSession` in multiplayer and applies them directly in
  single-player (unchanged). See `docs/plans/2026-07-03-multiplayer-networking-design.md`.
- **LAN lobby lifecycle, verified across two OS processes:** `opensupcom.exe
  --lan-host` + `opensupcom.exe --lan-join 127.0.0.1` run the real lobby handshake
  (host advertises scenario + RNG seed, client applies + readies, host fires the
  launch barrier) over a `MuxTransport` that carries both the lobby channel and the
  lockstep channel on one connection, then play a synced lockstep match. Both print
  matching scenario, seed, an RNG probe (proving the shared seed reached each sim —
  the fix for `weapon.cpp`'s previously non-deterministic firing randomness), and
  final checksum with `desynced=0`. Windowed reachability: `--lan-window-host` /
  `--lan-window-join <ip>` create the transport at startup and the game loop drives
  the same handshake to launch (a two-window play verified only by logic-equivalence
  to the headless run). See
  `docs/superpowers/specs/2026-07-04-windowed-lan-lobby-design.md`.
- **Player-drop / timeout handling, verified across two OS processes:** if a peer
  disconnects or freezes, `LockstepSession` declares it dropped after ~3s (30
  command frames) of missing confirmations, removes it from the scheduler gate so
  the survivor un-stalls, and reports it; the game loop defeats the dropped army
  (`SimState::defeat_army`, reusing the share-rule dispose path) so the match
  resolves instead of hanging. Verified: `--lan-join ... --mp-drop-at 20` makes the
  client leave mid-match; the host logs `peer source 1 timed out (31 frames behind)
  — dropped`, continues to completion (`stalled=0 dropped=1`), while a normal
  no-drop match still syncs (`dropped=0`). Once the game has ended a quiet peer
  is only a player leaving the score screen (`SessionEndGame` stops that
  client's sim), so `defeat_army` leaves the result alone. See
  `docs/superpowers/specs/2026-07-04-mp-player-drop-design.md`.
- **LAN IP-entry UI:** a front-end "LAN Game" button opens a dialog (host-IP field +
  Host/Join/Close + status) that calls the `LanHost([port])` / `LanJoin(ip[, port])` /
  `LanNetStatus()` engine globals over the LAN lifecycle above. `--lan-ui-test`
  verifies the globals headlessly (`LanHost` creates a listening transport,
  `LanJoin("")` is rejected) and that the dialog Lua snippet parses + `pcall`-degrades
  gracefully. The dialog is built with FA `maui`/`UIUtil`; its actual rendering/click
  is verified only in a live window (no GUI automation in CI) and is fully
  `pcall`-guarded so any UI mismatch logs a warning rather than breaking the menu.
  See `docs/superpowers/specs/2026-07-04-lan-ip-entry-ui-design.md`.
- `build/Debug/opensupcom.exe --full-smoke-test --map "/maps/SCMP_009/SCMP_009_scenario.lua"` completes the lifecycle: front-end, lobby/reload, game, score, return-to-front-end. Its lobby phase now launches through an `InternalCreateLobby` instance and `lobby:LaunchGame(config)`.
- `build/Debug/opensupcom.exe --lobby-flow-test` boots the no-map front-end, triggers the real `ButtonSkirmish()` path, pumps UI control frames, and verifies hosted-lobby callbacks fire.
- `smoke_report.txt` is clean after the full-smoke run: 0 unique issues, 0 total occurrences.

## Implemented Since The Older Plans

Some historical plan checkboxes are stale. The following items from the April full-skirmish plan are already present in code:

- `Control:Disable/Enable/IsDisabled`
- `ItemList:AddItems/ClearItems/SetTitleText`
- single-player lobby `SendData`/`BroadcastData` loopback
- cloak/stealth/sonar stealth unit toggles
- `SetAutoMode` / `GetAutoMode`
- `OnAdjacentTo` callbacks on completed adjacent structures
- death weapon enable/disable methods
- `IssueKillSelf`
- `CreateVisibleAreaAtPoint`
- Movie control MPEG loading/playback path
- lobby slot config wiring for `Human`, `AIPersonality`, `Faction`, `Team`, `StartSpot`, `PlayerColor`, and `ArmyColor`
- `GameOptions.ScenarioFile` launch wiring through `LaunchSinglePlayerSession`, registry launch state, reload, and full-smoke session config
- full-smoke lobby launch now exercises the lobby communication object's `LaunchGame(config)` path instead of directly calling `LaunchSinglePlayerSession`
- headless lobby-flow smoke now exercises front-end `ButtonSkirmish()` through menu animation, lobby creation, host-game callback, and connection-established callback
- score-screen `ReturnToLobby` is registered in shared UI bindings and signals the return-to-lobby path
- front-end fallback globals no longer overwrite real shared UI bindings during no-map menu bootstrap
- UI `SetFocusArmy` is now a real shared binding, including `-1` observer-mode focus normalization
- validated teleport destinations for playable bounds, path passability, and footprint occupancy
- cloak participation in effective vision and weapon target acquisition
- active unit maintenance stall behavior for cloak, radar, sonar, omni, jammer, radar/sonar stealth, stealth fields, water vision, and owner-paid shields
- moho cloak/stealth helpers now update the same intel state used by visibility, blips, and maintenance shutdown
- sim-side focus army normalization for FA Lua's 1-based army ids
- classification of the known FA AI builder `deepcopy` diagnostic below the active log level

## Game Modes / Victory Conditions (updated 2026-09-23, M189)

With retail or FAF data, the scenario's own `/lua/victory.lua` decides the game,
as in Moho. Retail's `BeginSession` hook forks `CheckVictory`, which calls the
brains' `OnDefeat`/`OnVictory`/`OnDraw` (so retail's result UI appears) and then
`EndGame`. The engine then ends the session (`SessionIsGameOver`,
`NoteGameOver`) without pausing, and retail's score screen ends it for good
(`SessionEndGame`). `--victory-test` plays that through.

The engine's own adjudication (`SimState::update_victory`, below) runs only for
data without a victory script. It enforces the game modes with categories
matching FA's `lua/victory.lua`:

- **Assassination** (`demoralization`) — eliminated when the last `COMMAND`/ACU dies.
- **Supremacy** (`domination`) — `STRUCTURE + ENGINEER - WALL`: eliminated when no
  structure or engineer remains (the ACU counts as an engineer; mobile combat units
  and walls do not keep you alive).
- **Annihilation** (`eradication`) — `ALLUNITS - WALL`: eliminated when only walls
  (or nothing) remain.
- **Sandbox** — no elimination. FAF's `decapitation` is treated as an ACU-kill.

Game-over is team-aware: armies are grouped into alliance-connected teams and the
match ends when one team remains (victory) or zero remain (draw). A defeated player
whose ally survives no longer ends the game. On defeat, an army's units are handled
per the `Share` option (`ShareUntilDeath` destroys; `FullShare` → ally; `PartialShare`
→ structures+engineers to ally, rest destroyed; `Defectors` → enemy; `CivilianDeserter`
→ civilian). `FogOfWar=none` reveals the whole map; `CommonArmy`
(`Union`/`Common`/…) pools allied economy; `TeamShareOverflow=enabled` routes wasted
overflow to allies. Covered by `tests/test_victory.cpp`, `test_fow.cpp`,
`test_common_army.cpp`, `test_team_share_overflow.cpp`, `test_handicap.cpp`.
In that fallback, not yet modeled: FA's 15s allied-victory-request sustain, and
`TransferToKiller` (needs per-unit killer attribution).

Army stats use Moho's names and meanings, which retail's score threads read:
- `Units_History` counts units built, `Units_Killed` counts the army's losses,
  and `Enemies_Killed` counts its kills;
- the value built, lost and destroyed, commanders destroyed;
- the economy totals, rates and waste, and the unit cap.

`GetBlueprintStat` splits them by category.

## Known Gaps And Risks

- **Multiplayer:** the lockstep session, lobby handshake, command routing,
  desync detection and peer drop all work across two processes. They are
  exercised on every CI run (`ctest -L mp`). Every player input reaches the
  sim as a command applied inside a tick, on every peer on the same tick
  (M198): orders, SimCallbacks, and the orders panel's unit settings. Still missing:
  pipelined command delay, slot and faction sync, and LAN discovery. A
  dropped player is decided by the survivors' agreement (M198b); two players
  dropping at once can still leave them disagreeing. Cross-OS determinism
  is checked by hand, not in CI (CI has no game data). The sim walks entities in id order (M195) and draws all its
  randomness from one seeded stream (M196). Two processes play the same
  four-AI game identically (`data.determinism`), so object addresses don't
  leak into the outcome within one build. For platforms (M197), the sim's
  transcendental math is FDLIBM; contraction is off; Lua formats numbers
  with `std::to_chars`. CI shows it for a synthetic game: MSVC, GCC and
  Clang reach the same pinned checksum. On real game data, a Windows build
  (MSVC, run under Wine) and a Linux build play a recorded five-minute
  four-AI game identically at every tick (`tools/cross_os_replay.py`, M199c).
  See roadmap Phases D and G.
- **Replays:** every interactive game records itself (format version 4: its
  setup, the commands the sim applied and the checksum after every tick) and
  leaves `LastGame` in the profile's replays. Retail's replay dialog lists
  and opens them; a replay plays in the game UI as an observer (`--watch
  <file>` from the command line). `--record <file>` names the file and
  `--replay <file>` plays one headlessly, reporting the first tick that
  differs. Two gate tests hold this: `data.replay_roundtrip` and
  `data.replay_flow`.
- **Sim/user boundary:** the renderer reads only per-tick snapshots (M190).
  The UI state's unit bindings (`UserUnit:GetPosition`, `GetHealth`, ...)
  still read the live sim; they move over with M191's split of the bindings
  into sim and user sides. `InputHandler` (picking, orders, the build
  ghost) works on the live sim by design.
- **Props aren't script-class instances:** retail props, trees and wrecks
  should be instances of `Prop`, `Tree`/`TreeGroup` (`/lua/proptree.lua`) or
  `Wreckage` (`/lua/wreckage.lua`), but get only `moho.prop_methods`. So
  retail's `CreateWreckageProp` fails at `SetReclaimValues`, and a retail
  unit death leaves no scripted wreck. Tracked with the projectile script
  classes (M201).
- **Death weapons:** projectiles are not yet instances of their script
  classes (M201). So a death weapon's `PassDamageData` fails. An ACU's
  `OnKilled` then falls back to engine destruction, and its death blast does no
  damage.
- Some lobby options are still stored-but-unenforced in C++ (difficulty-tier cheat
  multipliers are consumed by FA's AI Lua rather than the C++ economy; PrebuiltUnits
  needs blueprint/map data). Now enforced: **NoRush** (units confined near their
  start for the first N minutes; `tests/test_no_rush.cpp`), **CommonArmy** (allied
  armies pool mass/energy each tick, off by default; `tests/test_common_army.cpp`),
  per-army **handicap** (`ArmyBrain::set_handicap` scales income; `ArmyGetHandicap`
  now returns the real value; `tests/test_handicap.cpp`), and **TeamShareOverflow**
  (a full teammate's wasted overflow flows to allies with room, off by default;
  `tests/test_team_share_overflow.cpp`).
- Several stubs remain intentionally cosmetic, multiplayer-only, debug-only, or deprecated. They should stay classified so gameplay blockers are not hidden among harmless no-ops.

## Recommended Work Order

See `docs/ROADMAP.md` (the phases, exit criteria and Definition of Done) and the
tactical plan for the active phase in `docs/superpowers/plans/`.
