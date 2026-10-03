# OpenSupCom Current State

Last reviewed: 2026-09-29 (through PR #230: saves as full state snapshots, the binding tail, M216b's audio, M214e's emitter parameters, M215e's jammer blips; see `docs/ROADMAP.md` §10 for the phases and priorities)

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

## Platforms, Data Targets and Measured Status (2026-09-29)

| | Status |
|---|---|
| Linux | GCC 16 and Clang 22, Ninja + vcpkg presets `linux-debug` / `linux-release` / `linux-asan`. Warning-clean with `-Wall -Wextra`: CI's Linux jobs (GCC 14, Clang 18) build first-party code with `-Werror` (`OSC_WERROR`). |
| Windows | MSVC presets unchanged. CI builds and tests them; not re-verified by hand since the Linux work. |
| Retail FA 3599 (Steam) | Found automatically through the Steam libraries. Boots via retail `bin/SupComDataPath.lua`: glob mounts, `/schook` hooks, LuaPlus `#` comments and size hints, and the plain `Categories` lists. Headless SCMP_009 runs 100 ticks with 0 Lua errors. Units run their own retail script classes. 4 retail AIs play 10 game-minutes with 0 Lua errors and about 100 units, fighting (`--ai-skirmish --ai-armies 4 --ticks 6000`). An ASan build of the same run is clean. Retail's own front end boots and reaches a hosted skirmish lobby. In a windowed game, retail's own game interface runs and draws: economy, score, avatars, unit view, orders and construction panels, command-mode clicks and the minimap window. The C++ HUD placeholders remain behind `--legacy-hud`. Preferences are retail's Lua `Game.prefs` (profiles, options, window positions) in `<config>/opensupcom/`; tests and captures keep them in memory. Audio plays FA's own XACT data through an app-owned cue engine (M216): interface sounds, EVA and the campaign's voice, retail's music cycling its tracks, unit and weapon sounds with FA's variations, categories, falloff and Angle curves, limits and crossfades; pausing with the game, the duck's ramp, Moho's listener and stereo panning; world sounds heard only where the player's army sees, and long waves streamed. It has been checked by data and timing tests only; a listening pass is still to do. The world is drawn between the sim's last two ticks (M190a): the sim still ticks at 10 Hz, but units, walk cycles, projectiles and overlays move every frame. Mods (M221) work as in Moho. `doscript` loads a script and its hooks, `/schook`'s and each active mod's, as one chunk. A game's mods (the lobby's `GameMods`) reach its sim and UI states before their blueprints load, and its replay carries them. The player's mods folder is found (a mod may be a `.zip`), and so are their own maps. Retail's lobby and mod manager pick a game's mods. |
| Gameplay fidelity (Phase E, M200–M209) | The engine provides the machinery retail's own scripts expect, rather than parallel C++ behaviour:<br>• **Weapons:** retail's weapon state machines (targets, racks and salvos, priorities, restrictions, turret slew and firing tolerance, posed muzzles).<br>• **Projectiles:** script classes with `OnImpact`, and ballistic arcs. Swept collision against terrain, water, units, props, shields and projectiles, filtered by scripts.<br>• **Props and wreckage:** script classes. Trees split, fall and sink; wrecks are made by retail's scripts.<br>• **Movement:** acceleration, braking, turning and reversing; collision separation; retail's `formations.lua`.<br>• **Pathfinding:** cheaper after M205.<br>• **Missiles:** silo missile builds and launches, and anti-missile weapons.<br>• **Beams:** collision beams.<br>• **Economy events:** their resources are drawn (teleport, OverCharge).<br>• **Work ranges:** build, repair, reclaim and capture reach, measured by Moho's footprint gap.<br>• **Orders:** `Issue*` appends to the queue.<br>• **Ferries:** a transport's route carries units from its beacon to the last point and back.<br>• **Unloads:** `IssueTransportUnloadSpecific` drops only the cargo in its category, chosen when it is given.<br>• **Factory assist:** a factory guarding a factory builds units from the guarded factory's queue.<br>• **Script orders:** retail's Lua tasks run as Moho runs them. `EnhanceTask` handles upgrades from the panel or the AI, and `TargetLocation` handles abilities such as the Eye of Rhianne's scry.<br>• **Saves:** a save is the whole sim state (every object, the Lua heap), signed per installation and restored in under half a second (M208c).<br>• **Campaign:** operations launch as retail's `SetupCampaignSession` does and play headless (M209a). Retail's front end runs from the main menu through operation select, the briefing, an operation, its result and score screen, to the next operation's briefing, with the profile's progress kept (M209b); campaign saves and the tutorial are unchecked.<br>Where the scripts left Moho's rules unclear, they come from the decompiled engine ([faf-re](https://github.com/Draiget/faf-re)). |
| Presentation fidelity (Phase F, M210–M217) | The world draws as FA's own shaders draw it, ported by hand from its `.fx` files, with Moho's order and rules from faf-re:<br>• **Map lighting and sky:** each map's sun, fill, multiplier and specular in FA's light formula; the map's sky dome (atmosphere, sky decals, cirrus).<br>• **Materials:** the four factions' unit materials, Seraphim's falloff, wrecks, FA's bloom from the glow in alpha, the build shaders and build-effect meshes, rigid skinning and hidden bones, the props' materials and clipped shadows.<br>• **Terrain:** the strata blend, projected and lit decals, scripts' decals and splats, glowing decals, Moho's screen-space terrain normals, and TTerrainGlow's lava.<br>• **Water:** FA's surface (four wave layers, Fresnel, the ramp), reflections of units, and the shoreline's waves; every retail map is read to its last byte.<br>• **Effects:** FA's beams, trails, particles (emission once a tick, analytic motion, five blends) and refracting particles; scripts' emitter parameters and curves (M214e), effects made at the player's fidelity (M214f).<br>• **Fog of war and icons:** units, projectiles and effects seen through the player's intel, blips and remembered structures, FA's strategic icons, counter-intel, jammers' fake blips (M215e).<br>• **Media:** movies on their own clock, their sounds, the splash screens.<br>• **Input and camera:** wx key codes and input capture, retail's key map and console, the console's session commands, Moho's camera (log zoom, zoom-driven pitch and FOV, timed moves, SimCamera waits), the window as FA's video options set it (windowed, full screen, vsync, HiDPI), and the options applied through their console variables.<br>No FA reference captures exist on this machine: the looks are checked against the shaders' formulas and by eye, not against the game. |
| FAForever data | Supported through `--faf-data` (or `~/.faforever`) and `--init`. Verified on FAF's release, 3839 (`deploy/faf`, 2026-08-28), without a FAF client: `tools/faf_regression.py --packaged` builds a FAF data folder from the public FAForever/fa repository as FAF's client installs it (its `init_faf.lua` over `gamedata/*.nx2`) and plays the long set (`--suite long`). On 2026-09-30, after three fixes found by those runs, all ran without a Lua error or a hang: SCMP_009 with 4 AIs for 18,000 ticks (2,588 units at the end), SCMP_001 with 8 for 12,000, SCMP_026 with 4 for 12,000, SCMP_028 with 8 for 9,000, and FA's operations X1CA_001, 002, 004, 005 and 006 for 6,000 each. SCMP_016's two AIs finished early, one defeated at tick 3,501 by FAF's own victory check. X1CA_003 has two script errors that are FAF's own: its `scenariotriggers.lua` calls a `ScenarioUtils.GetUnitsInArea` its ScenarioUtilities doesn't define. FAF players run operations through FAF's coop mod, not checked here. The fixes: FAF's victory condition (`/lua/sim/victorycondition/`) now decides FAF games, where the engine had judged them too; Guard orders leave out the units Moho's issue does (a factory told to guard itself had FAF's roll-off loop walk forever in X1CA_006); `--faf-data` with `--fa-path` runs FAF's init. FAF's client and ICE adapter are not tested here. |

| Metric | Value |
|---|---|
| Unit tests (Catch2) | <!-- metric:unit_test_cases -->875<!-- /metric --> test cases in a Linux build (Windows leaves out a few POSIX-only ones). CI builds and runs them on GCC, Clang, ASan and MSVC. |
| Two-process MP tests (`ctest -L mp`, data-free) | <!-- metric:mp_tests -->4<!-- /metric --> |
| Static analysis (`ctest -L lint`, LLVM 22) | clang-tidy ratchet at its baseline of <!-- metric:tidy_baseline -->31<!-- /metric --> triaged findings. Changed lines follow `.clang-format` (a moved file only where it changed). The library targets link without a cycle or a layer reaching up, and <!-- metric:arch_tests -->12<!-- /metric --> such checks run in a Linux build, all but this document's own check on Windows too (`ctest -L arch`). |
| Data-backed gate on retail (`ctest -L gate`) | All <!-- metric:gate_tests -->204<!-- /metric --> pass: the data modes (including the no-map lobby flow, `--gameui-test`, `--victory-test`, the offscreen `--interp-test`, each Phase E system's own mode, e.g. `--missile-test`, `--beam-weapon-test`, `--range-test`, `--ferry-test`, and Phase F's offscreen render modes, e.g. `--sky-test`, `--water-render-test`, `--decal-render-test`, `--camera-moves-test`, `--options-test`), `data.determinism` (two processes play a four-AI game identically, compared domain by domain), `data.replay_roundtrip` and `data.replay_flow`, `data.save_load` and `data.load_flow` (M208a), `data.campaign_flow` (M209b), the `data.binding_coverage` ratchet, and <!-- metric:golden_tests -->5<!-- /metric --> golden captures, of FA's game interface at frame 600 and of retail's skirmish lobby and its Game Options dialog opened from the menu (`--click`), its map list paged (`--click-at`) (0.1% tolerance). New engine rules are mutation-checked: removing a rule makes its test fail. |
| Data-backed modes failing on retail (`-L retail-gap`) | None. The last six closed with engine fixes: blueprints are read from the store, not FAF's `self.Blueprint`; `GiveStorage` persists; finished or paused animations hold their pose; `EnableIntel` ignores intel a unit lacks (retail `SetupIntel` had been cloaking every unit); `CanBuild` reads category names. Tests that assumed FAF-only script fields were also fixed. |
| Long retail runs (Release, 2026-09-29) | No Lua errors in any of them. Skirmishes of retail AIs: SCMP_009 with 4 for 18,000 ticks (30 game-minutes, 28,000 entities by the end, 124 s), SCMP_001 with 8 for 12,000, SCMP_016 with 2 for 18,000, SCMP_026 with 4 for 12,000, SCMP_028 with 8 for 9,000. The six Forged Alliance operations, X1CA_001 to X1CA_006, for 6,000 ticks each (headless, no player input). One script thread in X1CA_002 had died on its first tick: the op destroys a wrecked base's structures as it makes them, and a neighbour's adjacency effect read one's blueprint a tick later. Moho keeps a destroyed entity until the end of the beat; the engine now does too (#232). The set ran again on 2026-09-30 with Moho's Guard order filter: all clean. FAF's release has run it too; the FAForever data row above has both. |
| Retail-only engine API still unbound | <!-- metric:unbound_globals -->17<!-- /metric --> globals and <!-- metric:unbound_methods -->7<!-- /metric --> methods (`opensupcom --binding-coverage`, ratcheted by `tests/integration/binding_baseline_retail.txt`). 13 of them are not in retail's engine either (script slips and dead paths, marked in the baseline); most of the rest are UI-only. |
| Benchmark (Release, four retail AIs, SCMP_009) | About 21 s for 6,000 ticks (it was 25.2 s before M205's path-cost fix). An 18,000-tick game runs without Lua errors. Its sim took about 256 s after M224b, against 450 s before M224 (compiled category matching, then unit-only radius queries; measured side by side). M224d–M224f (emitters end, Moho's Lua collection schedule, cheaper AI placement queries), with threads and effects now holding their Lua handles, bring the whole run to about 69 s, against 147 s before M224d. Late in that game about 1,300 units are alive. The renderer benchmark (M223b, `tools/bench.py --scenario render-*`) renders three scenes of the pinned game at 1920×1080: CPU `render()` p50 6-8 ms (p95 9-14 ms), GPU under 1 ms on an RTX PRO 4500. The frame is CPU-bound in its per-frame updates, the UI's and the units' (`docs/plans/2026-09-30-m223b-render-benchmark-design.md`). |

The counts above are a Linux build's, written by `tools/status_metrics.py update --build-dir <build>`; `arch.status_metrics` (Linux and macOS) fails when they are stale.

## Verified Locally

- `build/linux-debug/tests/osc_tests` passes (see the metrics above); `build/linux-debug/opensupcom --help` lists the CLI surface, including every `--*-test` mode.
- **Launched by a matchmaking client (GPGNet, M220a):** `opensupcom /gpgnet 127.0.0.1:<port>` connects to the client and carries out its commands (`CreateLobby` through retail's `onlineprovider.lua`, `HostGame`, `JoinGame`, `ConnectToPeer`...), telling it `GameState Idle` and `Lobby`; `data.gpgnet_lobby` checks it against a stand-in client, and `data.gpgnet_game` has the stand-in drive two games through retail's auto-lobby, as FAF's matchmaker does, to a launched game both play in lockstep; `data.gpgnet_game_relayed` does it with the two reaching each other through a UDP relay standing in for FAF's ICE adapter, losing 5% of what it carries. FAF's own client and ICE adapter aren't tested here (none is installed).
- **Windows and Linux in one game (CI):** the `cross-os-play` job plays the data-free lockstep pairs with the Linux build hosting the Windows build under Wine and the other way round (`tests/integration/cross_os_pairs.py`): in sync, a divergence caught, a vanished joiner dropped, a slow joiner setting the pace.
- **Cross-OS determinism on real data:** `tools/cross_os_replay.py --run-id <CI run>` plays a recorded four-AI game with the CI's Windows build (under Wine) and a Linux build; each Phase E PR has matched at every tick.
- **Multiplayer (LAN), retail's own screens to a game:** Multiplayer → LAN finds
  games (UDP discovery on port 15000), and retail's `lobby.lua` hosts, joins and
  launches over the engine's `CLobby` (`sim::LobbyNet`, through the host; Moho's "UDP" lobby over reliable UDP streams since M220c, "TCP" over TCP).
  `LaunchGame` hands the lobby's connections to the game, which plays in lockstep
  over them: pause (with the lobby's timeouts), the game's speed (as the lobby
  sets it, and players agree it), chat, `GetSessionClients`,
  `EjectSessionClient` and retail's disconnect dialog work as Moho's. See
  `docs/plans/2026-09-28-m218-lan-lobby-design.md`.
  - `data.lan_game` and `data.lan_game_quit` (need FA data) play retail's lobby
    in two processes from hosting to a game of 150 ticks: in step, pausing and
    resuming, chatting, the joiner raising the game's speed (adjustable in the
    lobby) and both following; and with the joiner leaving at tick 120, dropped
    by agreement, its army defeated, the dialog shown and closed.
  - The data-free CI pairs `mp.lockstep_sync`, `mp.lockstep_desync_detected`,
    `mp.lockstep_peer_drop` and `mp.lockstep_slow_peer` host and join a lobby
    over UDP, launch, and play minimal sims in lockstep through the game's own
    `route_command` path: in sync; an injected local divergence reported by
    both; a joiner that vanishes at round 20 dropped (after ~3 s, 30 rounds,
    without a word from it) while the host plays on; a joiner taking 200 ms a
    round setting the game's pace, with no one dropped (a peer runs at most two
    seconds of frames ahead of its sim, then waits). Once the game has ended a
    quiet peer is only a player leaving the score screen (`SessionEndGame` stops
    that client's sim), so `defeat_army` leaves the result alone.
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
- **Replays:** every interactive game records itself (format version 8: its
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
  and `data.load_flow` drives the dialogs' globals offscreen: a load from the
  front end, one from inside the game, a return to the lobby, and another.
- **A UI Lua state per game (M191 step 4):** the front end and each game get
  a fresh one, as Moho gives them. The old state goes with its controls,
  threads, beat functions and key maps. `FrontEndData` carries data across
  as copies. A second game in a run (lobby, game, lobby, game, or a load from
  the game menu) works as the first.
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
  - A repeating factory's queue shows an order split by its trip round the queue as two entries; Moho shows one with its count.
  - The AI's influence maps (M207b) take no false blips from jammers; nothing makes them yet. That is all M207 has left: `CheckBlockingTerrain` casts shots against the heightfield as Moho does (M207c).
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
8. M208 save/load: M208a done (saves as the game's history). M208b measured: a load replays the game, so it costs what playing it cost, and long games need M208c's snapshots.
9. ~~A first FAF regression run.~~ Done (`docs/plans/2026-09-25-faf-regression-run.md`). With #101 and #105, FAF's current game Lua plays 3,000 ticks without a script error. Next: longer runs, and presentation (Phase F).
10. Phase F, presentation: M210–M217 are done but for their listed gaps (`docs/ROADMAP.md`); FA's shields (M211k, M211l) are in review. Left: the water decals, animated decal textures, the low and medium fidelity variants, emitter parameters set at run time, XGS audio categories and ADX movie audio. Then Phase G (lobby completeness, cross-platform play, the FAF client).

The phases, exit criteria and Definition of Done are in `docs/ROADMAP.md`.
