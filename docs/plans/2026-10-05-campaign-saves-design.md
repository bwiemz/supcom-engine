# Campaign saves and the post-load (roadmap item 5, first part)

Design, 2026-10-05. M209b left operation select's Load, the tutorial and the last operation's outro. This covers the Load, and with it Moho's post-load, which campaign loads cannot do without.

## What fails today

- **Campaign saves don't work.** In a campaign, retail's Save and Load go through the `CampaignSave` special-file type:
  - the game menu's Save and Load tabs (`lua/ui/game/tabs.lua:180-195`, `gamemain.lua:675`);
  - operation select's Load (`lua/ui/campaign/selectcampaign.lua:106`).

  The engine knows only `Replay` and `SaveGame` (`src/lua/special_files.cpp`, `kTypes`). So `GetSpecialFiles('CampaignSave')` raises "unknown special file type", and the dialog breaks.
- **A loaded game never hears its post-load.** After a load, Moho runs `/lua/SimSync.lua`'s `SyncPlayableRect(playable rect)` and then the global `OnPostLoad()` (faf-re `Sim.cpp` ~7300). Retail's `schook/lua/simInit.lua` `OnPostLoad` re-sends to the new UI state what only the sim remembers:

  | Module | What its OnPostLoad re-sends |
  |---|---|
  | `SimUIState` | `Sync.CampaignMode` (UserSync sets `campaignmanager.campaignMode` from it), the transmission log, enhancement restrictions |
  | `SimObjectives` | every objective |
  | `SimDialogue` | open dialogues |
  | `SimPingGroup` | ping groups |
  | `ScenarioFramework` | finished dialogue flags |
  | `SimSync` | the focus army's unit data and `Sync.IsSavedGame` |

  So a campaign loads with no objectives, no transmission log, and campaign mode off: the UI then offers skirmish menus and saves the operation as a skirmish. It matters for skirmish too: unit data and `IsSavedGame`.

## Why the post-load is a design question

The post-load changes checksummed sim state. SimPingGroup forks a thread on every load, and the checksum's `threads` domain counts live threads. The engine promises two things that this breaks if it is called naively:

1. **A restored game plays on as the game it saved.** `data.save_load` compares a loaded game's checksum trace with the original's, domain by domain.
2. **A game's history replays it exactly.** A save made in a loaded game carries the whole game's history, and a replay, or a later load's catch-up, plays that history from tick 0. A post-load that isn't in the history would desync it.

## Design

**The post-load is a SimCallback in the game's history.** It is an engine-reserved callback, `__osc_PostLoad`, handled in `SimState::run_sim_callback` as Moho runs it:

1. `SimSync.SyncPlayableRect` of the playable area;
2. the global `OnPostLoad()`.

Script errors are logged, as Moho's `Warnf`.

- **When.** A load schedules it as the local player's callback (`schedule_callback`) as soon as the loaded game is the player's: right after a snapshot restore, or when a catch-up reaches the saved tick. It runs inside a tick, like any callback.
- **History.** It is recorded with the game's commands, so a replay, or a later catch-up through this history, runs it again at the same tick. Callbacks are already in the replay format (version 3), so the format doesn't change.
- **Reserved names.** Scripts can't issue it, or any `__osc_` callback. The UI's `SimCallback` binding refuses the prefix, which closes the same hole for the engine's existing `__osc_DefeatArmy` and friends.
- **Multiplayer.** Not affected: only single-player games load.

**`CampaignSave` type:** `{"CampaignSave", "savegames", "osccampaignsave"}`. Moho keeps campaign saves in the save-game folder under their own extension (faf-re `Sim.cpp` 1970-1996). The file is the same `SavedGame`. The game's setup already carries its `campaignInfo` (M209b), so a campaign save loads as the operation it was.

**The oracles.** A new `--post-load-at <tick>` makes a running game schedule the same callback at that tick, as a load would. `tests/integration/save_load.py` then compares like with like:

| Process | Change |
|---|---|
| A (plays and saves at SAVE_1) | runs with `--post-load-at SAVE_1` |
| B (loads A's save and saves again at SAVE_2) | runs with `--post-load-at SAVE_2` |
| C (loads B's save) | matches B after SAVE_2, as before |
| D, E (catch-up loads of A's save) | match A, as before |

`load_flow.py` and `replay_flow.py` need nothing new beyond "no Lua error".

**A loaded game's interface waits for its post-load.**

Moho runs the post-load at load time, so the world's first sync already carries `Sync.CampaignMode` when `gamemain.CreateUI` runs. That matters because CreateUI builds a campaign's objectives panel only in campaign mode; without the panel, `objectives2` queues the objectives for good.

The engine's post-load runs in the next tick instead, which keeps it in the history and replays it exactly. So a loaded game shows its loading screen until the post-load has run, and only then builds its interface (`App::world_ui_after_post_load`):
- a snapshot restore waits one tick;
- a catch-up waits until it is done, and then for the post-load it schedules.

The wait is for the load's *own* post-load. A save made in a loaded game holds that load's post-load in its history, and a catch-up through it replays it at its old tick; that must not bring the interface up. So the count to pass is taken when `App::post_load` schedules the load's post-load, and not when the load begins.

The player's input and the selection callbacks wait with it.

**Two Lua rules retail relies on**, now in the VM (`third_party/lua-5.0/lvm.c`):
- **Indexing a function gives nil.** Retail's `ScenarioFramework.PlayDialogue` reads `v.vid` of every field of a dialogue table, the dialogue's Callback among them, before it forks that callback. X1CA_001 has 37 dialogues with a callback; without this rule, none of their callbacks would run.
- **A field set on a boolean is dropped.** Retail's `diplomacy.lua` opens with `local parent = false; parent.Items = {}`, and its UserSync imports it for `Sync.SetAlliedVictory`, which retail's OnPostLoad sends after every load.

The engine already gave nil for indexing nil and booleans, for retail's UnitData entries that are `false`.

## Tests

- **Unit:**
  - `CampaignSave` resolves to `savegames/<profile>/<base>.osccampaignsave`, and each type lists its own files;
  - a Lua `SimCallback({Func = '__osc_DefeatArmy'})` is refused;
  - the VM's leniency rules hold.
- **`data.save_load` (and the replay and campaign variants)** pass with the oracle switches.
- **`data.load_flow`'s damaged-snapshot run** now catches up through the save made in a loaded game, whose history holds that load's post-load. The flow fails if the interface comes up while the catch-up runs.
- **`--campaign-flow-test` (`data.campaign_flow`) now saves and loads mid-operation.** Once X1CA_001 has given its objectives:
  1. the game is saved through `InternalSaveGame` to a `CampaignSave` path;
  2. `GetSpecialFiles('CampaignSave')` lists the save;
  3. it is loaded as the Load dialog loads it (`LoadSavedGame`);
  4. after the post-load, the new UI state has `campaignmanager.campaignMode` true and the same objectives;
  5. the operation plays on to its end, with no Lua error.

## Not here

- The tutorial and the last operation's outro (the rest of item 5).
- FAF's post-load list (`simInit.lua:567`) is FAF's own: it runs as written.
