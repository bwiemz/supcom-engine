# M209a: FA's Campaign Boots — Design

**Status:** 2026-09-29. Roadmap Phase E (M209, scenario scripting).

## Why

FA's operations are scripts: `ScenarioFramework`, the OpAI base managers, objectives,
NIS camera work, dialogue, and the faction pick. They call engine API that skirmish never
does. This milestone launches an operation the way retail does and closes the gaps its
scripts hit. It stops at a mission that plays, not at the campaign's front end.

## How an operation launches

Retail's `SetupCampaignSession` (`/lua/ui/campaign/campaignmanager.lua`) is the model. It
applies when a scenario's `type` is `campaign` and nothing else configures the armies,
such as a lobby, a replay or a save: `opensupcom --map /maps/X1CA_001/X1CA_001_scenario.lua`.

- **Armies:** the first is the player's. The others are the AI, with no personality: the
  operation's own scripts drive them (OpAI).
- **Options:**
  - `FogOfWar = 'explored'`
  - `Difficulty`, from `--difficulty 1`–`3` (default 2)
  - `DoNotShareUnitCap = true`
  - `Timeouts = -1`
  - `GameSpeed = 'normal'`
  - `FACampaignFaction = 'uef'` (the operation changes it when the player picks)
  - `Victory = 'sandbox'`: the operation's script decides the end.
- **Factions:** each army's comes from its save file.

## What the operations needed

| Area | API | Moho's behaviour (faf-re, retail scripts) |
|---|---|---|
| Armies | `GenerateArmyStart(army)` | Two draws of the sim's generator, × 0.8 / 2³² + 0.1 of the map's width and height: a start in the map's middle 80%. |
| | `SetAllianceOneWay(a, b, rel)` | Sets only `a`'s view of `b`. |
| | `SetArmyFactionIndex(army, i)` | Takes a 0-based index, as save files give it. `GetFactionIndex` answers 1-based. |
| | `SetIgnorePlayableRect(army, on)` | `CArmyImpl::UseWholeMap`. That army's units keep to the map, not the playable area. The engine's movement clamps are per army now. |
| Platoons | `CanFormPlatoon`, `FormPlatoon` | A squad names its units by blueprint id (compared without regard to case) or by category. Dead and being-built units don't count. `CanFormPlatoon` is false when the platoon would take no unit at all. |
| | `platoon:GetBrain()` | Answers during the platoon's `OnDestroy`, whose destroy callbacks get the brain. It had been nil, which broke the PBM's and the attack manager's callbacks. |
| Areas | `GetUnitsInRect`, `GetReclaimablesInRect` | Read a Rect by its `x0/y0/x1/y1` fields (`SCR_FromLuaCopy<Rect2f>`). The engine read array slots, so every Rect came back empty. `GetReclaimablesInRect` returns units and props, and nil for none, and also takes four numbers. |
| Blips | `unit:GetBlip(army)` | One object per unit and army, an instance of `/lua/sim/Blip.lua`'s `Blip`. Its destroy hooks are what objectives use. Its `OnDestroy` runs once its unit is gone. |
| | `blip:BeenDestroyed()` | True once its unit is gone, since the blip goes with it. It had read a cache the sim clears as the unit dies, so it never turned true. |
| | `unit:OnDetectedBy(army)` | Called when an army makes its first blip of a unit. Objectives and the experimental-detected voices hang off it. |
| | `blip:GetEntityId()` | A `ReconBlip` is an Entity. The engine's blip is its unit as an army sees it, so the id is the unit's. Cinematics tracks units by it. |
| Entities | `GetCollisionExtents()` | `{Min, Max}` of the collision shape's world bounding box, nil without a shape. Objective arrows sit on top of it. |
| | `SetStrategicUnderlay(icon)` | An icon drawn under the unit's strategic icon, at its own colour, while the unit is identified: the objectives' rings. |
| | `slider:BeenDestroyed()` | `CSlideManipulator`'s. Build effects check it. |
| | `OnDamage`'s vector | A real vector (`x/y/z`), from the origin to the target. The shield impact effect reads `.x`. |
| Movies | `GetMovieDuration(file)` | The Sofdec header's frames over the MPEG sequence header's rate. 0, with a warning, for a missing file or one that isn't a movie; the dialogue then falls back to `AllyCom.sfd`. |
| UI | `SyncPlayableRect(rect)` | The user side of the playable area. The camera keeps to it, and the meshes of entities outside it at that moment hide until the next sync. |
| | `RenderOverlayEconomy(on)` | The session's economy-overlay flag, which the MFD toggle sets and a NIS clears. Nothing draws the overlay yet. |

## What it changes in skirmish

Several of these fixes were in paths that retail's skirmish AI also takes:

- Engineers now find reclaim: `AIGetReclaimablesAroundLocation` passes a Rect.
- Area buffs find their targets: `GetEnemyUnitsInSphere` also passes a Rect.
- Platoon destroy callbacks run.
- `OnDetectedBy` fires.

AI games play differently as a result. The benchmark's checksum changes, and its baseline is
re-recorded once this merges.

## Found, not done here

- **Economy rates are per second; Moho's are per tick.** This affects `GetEconomyIncome`,
  `Requested`, `Usage` and `Trend`, and the UI's `GetEconomyTotals`.
  - Retail's economy bar multiplies by the tick rate, so it shows 10× the real income and
    drain.
  - Retail's AI build conditions compare `MassIncome * 10` against per-second drains, so its
    decisions are skewed too.
  - This is a separate fix, because it moves skirmish play by itself.
- **Adjacency timing (X1CA_002).** One adjacency-beam thread fails when an operation hands
  a structure to the player in the tick it's made: the neighbour is gone by the time the
  thread runs.
- **Blip lifetime.**
  - Moho deletes a mobile unit's blip when every sense loses it, and makes a new one, with
    `OnDetectedBy` again, when it's found.
  - The engine keeps a dead-reckoning blip until the unit dies. That belongs with the rest
    of the recon tail (Phase F).
- **Drawing:** the playable area's boundary (Moho's `BoundaryRenderer`) and the economy
  overlay.
- **The campaign's front end:** operation select, briefings, progress and unlocks, and
  carrying a game between operations.

## Verified

- **X1CA_001:**
  - Headless for 3,000 ticks with no script error.
  - Windowed past the intro NIS, the dialogue with its movie lookups, and the faction pick
    (answered from a scratch UI hook). The game then runs with its objectives and their
    arrows and rings.
- **X1CA_003:** clean over 1,500 ticks.
- **X1CA_002:** clean over 1,500 ticks apart from the adjacency-timing error above.

## Tests

- **`--campaign-test` (gate).** X1CA_001 boots with:
  - its armies and brain types;
  - the campaign options;
  - the save file's factions, with 0-based `SetArmyFactionIndex`;
  - one-way alliances;
  - `GenerateArmyStart` in bounds;
  - the playable area with the other armies ignoring it.

  It then runs 600 ticks. Any script error fails it.
- **Unit tests:**
  - `sofdec_duration`;
  - the playable rect's clamp and its hiding rule;
  - the collision shape's world box;
  - the per-army playable clamp.
- **Data tests:**
  - platoons by blueprint id, and `CanFormPlatoon` with nothing to take;
  - blips as one object per army, with `OnDetectedBy` and `OnDestroy` hooks, and their
    `GetEntityId`;
  - `GetUnitsInRect` with a Rect;
  - `GetReclaimablesInRect`'s semantics;
  - `GetCollisionExtents`;
  - the `OnDamage` vector;
  - `GetMovieDuration` against the retail movies;
  - underlays drawn under strategic icons.
- **Binding coverage:** seven names come off the retail baseline.
