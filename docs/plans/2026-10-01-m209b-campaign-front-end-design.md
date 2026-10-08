# M209b: FA's Campaign Front End — Design

**Status:** 2026-10-01. Roadmap Phase E (M209), the item M209a left open.

## Why

M209a launched an operation from the command line. A player starts one from the main menu.
That path runs these screens:

1. operation select;
2. the briefing;
3. the operation;
4. its result;
5. the score screen;
6. the next operation's briefing.

All of them are retail Lua: `selectcampaign.lua`, `operationbriefing.lua`,
`campaignmanager.lua`, `score.lua` and the schook `UserSync.lua`. The engine's part is the
bindings and the hand-overs those scripts expect. This milestone walks the path as a player
would and fixes what breaks, each fix against Moho's own code (faf-re).

## The flow, as retail wires it

- **The main menu's Campaign** opens `selectcampaign.CreateUI()`.
  - On a profile's first visit (`ViewedTimeline` unset), it plays the timeline movie
    instead.
  - A click skips the movie to X1CA_001's briefing. Watching it to its end leads to
    operation select.
- **The briefing's Launch** calls
  `LaunchSinglePlayerSession(SetupCampaignSession(scenario, difficulty, faction, {opKey,
  campaignID, difficulty}))`. The fourth argument becomes `ScenarioInfo.campaignInfo`.
- **In the operation:**
  - Its armies set `Sync.CampaignMode`.
  - Its intro is a NIS (`Sync.NISMode`).
  - X1CA_001 asks for the faction (`Sync.RequestPlayerFaction`), which a SimCallback
    answers.
- **On a win,** `ScenarioFramework.EndOperation` syncs `OperationComplete`, from which
  `UserSync` calls `OperationVictory`. That function:
  - records the profile's progress (`campaign`);
  - sets `FrontEndData.NextOpBriefing`;
  - shows "Operation completed".
  Its Ok opens the score screen, whose Continue calls `ExitGame`.
- **Back in the front end,** `uimain.StartFrontEndUI` sees `NextOpBriefing` and opens the
  next operation's briefing.

## What was wrong, and Moho's rule for each

- **The launch** (`LaunchSinglePlayerSession`) read only a lobby's config
  (`GameOptions.ScenarioFile`). A single-player session (`SinglePlayerLaunch.lua`'s, as Moho's
  `WLD_SetupSessionInfo` takes it) names its scenario as `scenarioInfo.file`, its players as
  `teamInfo` and its options as `scenarioInfo.Options`. It carries `campaignInfo` and
  `tutorial` in `scenarioInfo`, which becomes the sim's `ScenarioInfo`.
  - The engine now reads that shape.
  - `campaignInfo` and `tutorial` travel in the game's setup (replay format 12), so a replay
    or a saved game keeps them.
- **The first sync** must reach the UI before the game interface is built. Moho's
  `WLD_DoInitializing` applies it (`DoBeat`), then calls `StopLoadingDialog`, then
  `CreateGameInterface`. `gamemain`'s `CreateGameInterface` reads `campaignMode` from that
  sync, and `HideGameUI` toggles:
  - built before the sync, a campaign's interface was hidden once on creation;
  - the intro's `NISMode('on')` then showed it again.
  The engine now applies the first sync first.
- **Hiding** (`CMauiControl::SetHidden`, which `Hide` and `Show` call):
  - the flag is set, then `OnHide(hidden)` runs; if it returns true, the children stay as
    they are (retail's `Grid` shows its cells itself);
  - otherwise each child goes through the same.
  The engine's `Hide`/`Show` skipped `OnHide`, and `SetHidden` didn't recurse, so a
  `Window`'s border (a group beside it, hidden by its `OnHide`) stayed up.
- **Bitmap sizes:** `CMauiBitmap::SetTexture` sets the `BitmapWidth`/`BitmapHeight` LazyVars
  (from a batch's first texture). Width and Height follow them through retail's
  `ResetLayout`, unless a script sized the control itself.
  - The engine set Width and Height directly instead. Every texture change undid a script's
    sizing: operation select's half-size faction icons spilled over their rows, the unit
    view's 48×48 portrait and 188×16 health bar grew, and the score panel lost two icons.
  - `Width:Set(control.BitmapWidth)`, retail's other idiom, had followed a method called
    without its control, which returned 0.
- **Hit-test ties:** `CMauiControl::GetTopmostControl` walks depth-first, children in the
  order they were made, and keeps a control only for a strictly deeper one. So on equal
  depth the first one wins.
  - The engine's `>=` let the last win.
  - The score screen's page group, made after Continue at the same depth, took its clicks.
- **Movies are hit-tested** like any control. The engine disabled it for every movie (from
  April, when hit-testing took the last control in tree order). The timeline movie's skip
  needs the clicks. Retail disables hit-testing itself on the movies that are backgrounds.
- **The NIS console switches** `ui_RenderUnitBars` (life bars), `ui_NisRenderIcons`
  (strategic icons) and `ren_SelectBoxes` (selection marks) were unknown commands. They now
  gate the engine's overlay and icons.
- **Teardown:** returning to the front end destroyed the sim's Lua state. Its
  `BlueprintStore` kept pointing at it until a next game rebound it. Quitting from the menu
  after a game released those references in freed memory and crashed. The store is now
  unbound with its state.
- **The player's files:** a scripted window (a flow test) counted as interactive, so it
  defaulted to the player's `Game.prefs` and FA user folder. It now has them only when
  given them (`--prefs`, `--user-dir`). It never writes a prefs file back, so a test's
  fixture can't change either.

## Tests

- **`--campaign-flow-test`** (`data.campaign_flow`, gate), offscreen, through retail's
  screens with a player's clicks:
  1. Campaign; the timeline movie, skipped by a click; X1CA_001's briefing; Back.
  2. Operation select: Select, then Launch.
  3. During the intro: the interface is in campaign mode and hidden. Then the faction
     dialog's UEF.
  4. `ScenarioInfo.campaignInfo` is checked (X1CA_001, difficulty 2, UEF), and the
     operation is ended as its scripts end it on a win.
  5. Ok, then Continue.
  6. X1CA_002's briefing opens, with X1CA_001 finished and X1CA_002 unlocked in the
     profile.

  Each fix it covers fails it when reverted.
- **Unit tests:**
  - hit-test ties;
  - `SetHidden` with `OnHide` (recursion, a kept control, a `Window`-like border);
  - movies hit-tested.
- **`--bitmap-test` Test 13:** sizes that follow the texture, a half-size function, a fixed
  size.

## Left

- The `Cam_Free` and `range_Render*` console commands (`gamemain` sets them; no visible
  effect yet). *Done (2026-10-08): they exist from startup, so a `--map` game's interface,
  built before the window, keeps what it sets; its range profiles too.*
- Operation select's Load, and campaign saves: `simuistate.OnPostLoad` re-syncs
  `CampaignMode` after a restore. That path is unchecked.
- The tutorial (X1CA_TUT, `tutorial = true`) and the final operation's outro and credits
  movies: launched by the same code, but unplayed.
- Playing each operation through: their scripts, not this front end.
