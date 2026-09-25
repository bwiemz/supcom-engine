# OpenSupCom Current State

Last reviewed: 2026-09-24 (through M192 step 1; M193, M191 step 1 and the checksum domains merged as PRs #65–#68; see `docs/ROADMAP.md` for the plan)

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

## Platforms, Data Targets and Measured Status (2026-09-24, main at M193, M192 in review)

| | Status |
|---|---|
| Linux | GCC 16 and Clang 22, Ninja + vcpkg presets `linux-debug` / `linux-release` / `linux-asan`. Warning-clean with `-Wall -Wextra`: CI's Linux jobs (GCC 14, Clang 18) build first-party code with `-Werror` (`OSC_WERROR`). |
| Windows | MSVC presets unchanged. CI builds and tests them; not re-verified by hand since the Linux work. |
| Retail FA 3599 (Steam) | Found automatically through the Steam libraries. Boots via retail `bin/SupComDataPath.lua`: glob mounts, `/schook` hooks, LuaPlus `#` comments and size hints, and the plain `Categories` lists. Headless SCMP_009 runs 100 ticks with 0 Lua errors. Units run their own retail script classes. 4 retail AIs play 10 game-minutes with 0 Lua errors and about 100 units, fighting (`--ai-skirmish --ai-armies 4 --ticks 6000`). An ASan build of the same run is clean. Retail's own front end boots and reaches a hosted skirmish lobby. In a windowed game, retail's own game interface runs and draws: economy, score, avatars, unit view, orders and construction panels, command-mode clicks and the minimap window. The C++ HUD placeholders remain behind `--legacy-hud`. Preferences are retail's Lua `Game.prefs` (profiles, options, window positions) in `<config>/opensupcom/`; tests and captures keep them in memory. Audio plays FA's own XACT data through an app-owned cue engine: interface sounds, retail's music thread, unit and weapon sounds with FA's categories, falloff curves and limits. It has been checked headless only; a listening pass is still to do. The world is drawn between the sim's last two ticks (M190a): the sim still ticks at 10 Hz, but units, walk cycles, projectiles and overlays move every frame. |
| Gameplay fidelity (Phase E, M200–M206) | The engine provides the machinery retail's own scripts expect, rather than parallel C++ behaviour:<br>• **Weapons:** retail's weapon state machines (targets, racks and salvos, priorities, restrictions, turret slew and firing tolerance, posed muzzles).<br>• **Projectiles:** script classes with `OnImpact`, and ballistic arcs. Swept collision against terrain, water, units, props, shields and projectiles, filtered by scripts.<br>• **Props and wreckage:** script classes. Trees split, fall and sink; wrecks are made by retail's scripts.<br>• **Movement:** acceleration, braking, turning and reversing; collision separation; retail's `formations.lua`.<br>• **Pathfinding:** cheaper after M205.<br>• **Missiles:** silo missile builds and launches, and anti-missile weapons.<br>• **Beams:** collision beams.<br>• **Economy events:** their resources are drawn (teleport, OverCharge).<br>• **Work ranges:** build, repair, reclaim and capture reach, measured by Moho's footprint gap.<br>• **Orders:** `Issue*` appends to the queue.<br>• **Ferries:** a transport's route carries units from its beacon to the last point and back.<br>Where the scripts left Moho's rules unclear, they come from the decompiled engine ([faf-re](https://github.com/Draiget/faf-re)). |
| FAForever data | Still supported through `--init`/`--faf-data` or `~/.faforever`. Not re-verified: this machine has no FAF install. |

| Metric | Value |
|---|---|
| Unit tests (Catch2) | 461 cases / 39,153 assertions (the RNG tests draw many values). CI builds and runs them on GCC, Clang, ASan and MSVC. |
| Two-process MP tests (`ctest -L mp`, data-free) | 5/5 |
| Static analysis (`ctest -L lint`, LLVM 22) | clang-tidy ratchet at its baseline of 33 triaged findings. Changed lines follow `.clang-format` (a moved file only where it changed). The library targets link without a cycle or a layer reaching up (`ctest -L arch`). |
| Data-backed gate on retail (`ctest -L gate`) | All 129 pass: 121 data modes (including the no-map lobby flow, `--gameui-test`, `--victory-test`, the offscreen `--interp-test`, and each Phase E system's own mode, e.g. `--missile-test`, `--beam-weapon-test`, `--range-test`, `--ferry-test`), `data.determinism` (two processes play a four-AI game identically, compared domain by domain), `data.replay_roundtrip` and `data.replay_flow`, `data.save_load` and `data.load_flow` (M208a), the `data.binding_coverage` ratchet, and two golden captures of FA's game interface at frame 600 (0.1% tolerance). New engine rules are mutation-checked: removing a rule makes its test fail. |
| Data-backed modes failing on retail (`-L retail-gap`) | None. The last six closed with engine fixes: blueprints are read from the store, not FAF's `self.Blueprint`; `GiveStorage` persists; finished or paused animations hold their pose; `EnableIntel` ignores intel a unit lacks (retail `SetupIntel` had been cloaking every unit); `CanBuild` reads category names. Tests that assumed FAF-only script fields were also fixed. |
| Retail-only engine API still unbound | 43 globals and 24 methods (`opensupcom --binding-coverage`, ratcheted by `tests/integration/binding_baseline_retail.txt`). Many are UI-only. |
| Benchmark (Release, four retail AIs, SCMP_009) | About 21 s for 6,000 ticks (it was 25.2 s before M205's path-cost fix). An 18,000-tick game runs without Lua errors. |

## Verified Locally

- `build/linux-debug/tests/osc_tests` passes (see the metrics above); `build/linux-debug/opensupcom --help` lists the CLI surface, including every `--*-test` mode.
- **Cross-OS determinism on real data:** `tools/cross_os_replay.py --run-id <CI run>` plays a recorded four-AI game with the CI's Windows build (under Wine) and a Linux build; each Phase E PR has matched at every tick.
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
- `osc_integration --full-smoke-test --map "/maps/SCMP_009/SCMP_009_scenario.lua"` completes the lifecycle: front-end, lobby/reload, game, score, return-to-front-end. Its lobby phase now launches through an `InternalCreateLobby` instance and `lobby:LaunchGame(config)`.
- `osc_integration --lobby-flow-test` boots the no-map front-end, triggers the real `ButtonSkirmish()` path, pumps UI control frames, and verifies hosted-lobby callbacks fire.
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
- **Saved games (M208a):** a save is the game's recording, plus the orders
  still to run. Retail's Save and Load dialogs and quick-save work through
  `InternalSaveGame` and `LoadSavedGame`. A load catches up to the saved tick
  in the game's frame loop, checked against the save's checksums, and then
  the game is the player's again. Saves from another build are refused as
  `WrongVersion`. `--load`, `--save` and `--save-at` do the same headlessly.
  `data.save_load` chains three processes (save, load and save again, load),
  and `data.load_flow` drives the dialogs' globals offscreen. Loading from
  inside a running game isn't reliable yet. Any game started from another
  needs a fresh UI Lua state, as retail gives each game, and the renderer
  leaks on relaunch (see the M208 design).
- **Sim/user boundary:** the renderer reads only per-tick snapshots (M190).
  The UI state's units are Moho's `UserUnit` (M191 step 3). They read the
  tick's snapshot, and change the sim only through the command stream.
  `GetStat` and `CanAttackTarget` read the live unit (read-only). `InputHandler`
  (picking, orders, the build ghost) works on the live sim by design.
- **Architecture (Phase C):**
  - `Unit::update` runs in five named phases, and each order kind has its own handler in `src/sim/unit_orders.cpp` (M193).
  - The library cycle is broken (M191 step 1). The UI bindings that need the renderer live in `osc_lua_user`, and `arch.link_layers` guards the layering.
  - `moho_bindings.cpp` is split by class into `src/lua/bindings/{sim,ui}/` (M191 step 2).
  - The UI's units are Moho's `UserUnit` (M191 step 3). They read the tick's snapshot (the renderer's capture in a drawn game) and change units only through the command stream, as `SetCustomName` now does: it travels as Moho's `CustomName` ProcessInfo pair.
  - The game is `osc::app::run` (`src/app/`). Its test modes are the integration runner, `osc_integration`, which CTest runs (M192 step 1).
  - `app.cpp` is split by concern (`cli`, `ui_globals`, `session`, `reload`, `frames`) and keeps only `run()` (M192 step 2a).
  - `run()` is an `App` object with the boot, windowed loop and headless run as its methods, each in a file of its own (M192 step 2b).
  - Next: M192 step 2c, the windowed loop's frame broken into handlers.
- **Determinism diagnostics:** the per-tick checksum has 11 domains: RNG, armies, entities, units, orders, navigation, weapons, projectiles, shields, economy events and script threads. `--checksum-trace` writes each one, and a lockstep desync names the domains that differ. Of the scripts' state it hashes only which threads live and when each wakes, not Lua tables.
- **Multiplayer robustness:** a wire message is capped at 4 MiB (a peer claiming more is dropped), and a peer's orders and SimCallbacks move only its own army's units. Peers are not yet authenticated.
- **Order fidelity gaps (after M206):**
  - A factory assisting a factory lends nothing; Moho copies its queue.
  - Transports don't land, and their capacity isn't read from attach points.
  - `IssueTransportUnloadSpecific` drops every unit.
  - A surfaced sub's torpedoes don't dive, and a dived sub sinks only while moving.
  - Platoons formed by `FormPlatoon` don't set `PlatoonHandle` (M207).
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

After M206, as agreed on 2026-09-24:
1. Keep these docs current.
2. ~~Network hardening.~~ Done (#63).
3. ~~A checksum split by domain.~~ Done (#65).
4. ~~M193: split `Unit::update`.~~ Done (#68).
5. M192: split the executable. Step 1 in review (#69, with M191 step 2); step 2 decomposes `app.cpp`.
6. ~~M191: finish the Sim/User split.~~ Done: the cycle is broken (#66), the bindings are split by class (step 2), and the UI's units are UserUnit, reading snapshots and changing the sim through the command stream (step 3).
7. M206's remaining gaps, and M207.
8. M208 save/load: M208a done (saves as the game's history). Then M208b (load times), a first FAF regression run, and presentation (Phase F).

The phases, exit criteria and Definition of Done are in `docs/ROADMAP.md`.
