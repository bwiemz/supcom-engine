# Engine API gaps

Engine API that retail's Lua and data use and the engine doesn't support, or supports in part, with what FAF adds on top. Found by audits of the bindings' optional and flag arguments, the callbacks' return values, the data's flags and categories, the parity-ratchet baselines and FAF's executable patches, and by play. Checked against main at be5c57c (2026-10-09).

How to read it:
- Each group lists retail's gaps first, ranked by how visible they are in play, then FAF's additions.
- **Missing** gives retail's rule and what the engine does instead. faf-re is decompiled from FAF's patched executable; each rule cited from it was checked against retail's, and rows that follow FAF's patches say so. faf-re paths are relative to `src/sdk/moho`.
- **Effect** follows the path from retail's Lua through the engine's code to what the player sees; *seen* marks gaps also observed in play on this engine.
- **Status:** *Open*, an open PR, or the PR that closed it. Closed items are listed at the end.
- The ratchet baselines (`tools/*_baseline.txt`, `tests/integration/binding_baseline_retail.txt`) list every unread field, stub and unmade callback, and `docs/plans/2026-10-05-parity-ratchet-triage.md` rates them all. Only those with an effect traced here are repeated.

## Sim Lua API

| Missing | Effect | Evidence | Status |
|---|---|---|---|
| `GetBoneDirection(bone)` returns three numbers `x, y, z` in retail; the engine returns a vector table | The Scathis never fires: its `CreateProjectileAtMuzzle` reads the three numbers, errors in `GetAngleInBetween` before the shell is made, and the weapon's thread dies | faf-re `cfunc_EntityGetBoneDirection` (`entity/Entity.cpp`); retail `/units/URL0401/URL0401_Script.lua` CreateProjectileAtMuzzle; engine `entity.cpp` entity_GetBoneDirection | Open |
| `FlattenMapRect(x, z, sx, sz, y)` writes the heightfield in retail; the engine's is a no-op | Terrain under structures with a FlattenSkirt (219 retail blueprints) isn't levelled; buildings stand on the slope | faf-re `cfunc_FlattenMapRect` (`sim/Sim.cpp`); retail `/lua/defaultunits.lua` OnCreate; engine `sim_bindings.cpp` stub_noop | Open |
| `CreateAnimator(unit, true)` ties the animation's rate to the unit's speed in retail (step × speed / MaxSpeed); the engine ignores the second argument | Walk cycles play at a fixed rate, so feet slide when a walker is slower than its MaxSpeed | faf-re `cfunc_CreateAnimator`, `CAnimationManipulator` (`animation/CAnimationManipulator.cpp`); retail `/lua/defaultunits.lua`; engine `sim_bindings.cpp` l_CreateAnimator, `sim/manipulator.cpp` | Open |
| `MotorFallDown:Whack` tips a tree over tick by tick in retail; the engine turns it 90° at once. Retail calls a prop's `OnCollision` when a unit runs into it; the engine has no unit-prop collision | Trees hit by fire fall flat in one tick; tanks driving through a forest don't knock trees over (`proptree.lua` OnCollision → FallDown → Whack) | faf-re `MotorFallDown` (`entity/MotorFallDown.cpp`), the prop's `OnCollision` (`sim/Sim.cpp`); retail `/lua/proptree.lua`; engine `entity.cpp` falldown_Whack | Open |
| `Unit:RequestRefreshUI()` makes retail's session re-set the selection so `OnSelectionChanged` runs; the engine's is a no-op | A selected unit's orders and build panels go stale after an enhancement, an adjacency change or a transport load/unload, until it is reselected | faf-re `RequestRefreshUI` (`unit/core/Unit.cpp`, `UserUnit.cpp`); retail `/lua/sim/Unit.lua`, `/lua/defaultunits.lua`; engine `entity.cpp` stub_noop | Open |
| `brain:GetUnitsAroundPoint(cat, pos, r)` without an alliance returns any army's units in retail (foreign ones when the brain has a blip of them); the engine returns only the brain's own | The Ythotha's energy being (XSL0402) fires only at its own side's land units, never at the enemy | faf-re `cfunc_CAiBrainGetUnitsAroundPoint` (`ai/CAiBrain.cpp`); retail `/lua/seraphimunits.lua`; engine `aibrain.cpp` "Default: own army only" | Open |
| `brain:GetListOfUnits(cat, needToBeIdle, requireBuilt)`: retail's `requireBuilt` is the optional third argument, default true; the engine reads the second as `requireBuilt` and never filters idle | With `(cat, true)` busy units count as idle (AI idle-count conditions, `FindIdleGates`); with `(cat, false)` unfinished units are included, e.g. `OnDefeat`'s KillArmy also kills the army's half-built units | faf-re `cfunc_CAiBrainGetListOfUnits` (`ai/CAiBrain.cpp`); retail `/lua/aibrain.lua` OnDefeat, `/lua/ScenarioFramework.lua`, `/lua/AI/aiutilities.lua`; engine `aibrain.cpp` | Open |
| `Prop:AddBoundedProp(priority)` ranks wrecks by mass in retail and destroys the lowest whenever there are 1000; the engine's is a no-op | Wrecks pile up with no cap through a long game; retail never holds more than 1000. FAF also dropped the call | faf-re `AddBoundedProp` (`entity/Prop.cpp`, `entity/EntityDb.cpp`); retail `/lua/sim/Unit.lua` CreateWreckageProp; engine `entity.cpp` | Open |
| `CreateThrustController`'s manipulator swivels engine bones with thrust in retail (`SetThrustingParam`'s 8 parameters); the engine's never turns them | UEF transports and gunships (UEA0104, UEA0107, UEA0203, UEA0305, XEA0306): nozzles stay in their animation pose | faf-re `CThrustManipulator` (`animation/CThrustManipulator.cpp`); retail `/units/UEA0107/UEA0107_Script.lua`; engine `manipulator.hpp` (ThrustManipulator::tick empty) | Open |
| `platoon:MoveToLocation(pos, useTransports, squad)` orders only that squad in retail; the engine orders every unit | Campaign base transports: the engineers aboard also get the transports' route, and after landing walk back through its waypoints before they build | faf-re `cfunc_CPlatoonMoveToLocation` (`sim/CPlatoon.cpp`); retail `/lua/ScenarioPlatoonAI.lua` StartBaseTransports; engine `platoon.cpp` platoon_order_to | Open |
| `IssueFormAttack(units, target, formation, heading)` attacks in formation in retail; the engine's is a plain `IssueAttack` | A skirmish AI attack platoon's final push on a nearby target goes in without a formation | faf-re `cfunc_IssueFormAttack` (`sim/CCommandLuaFunctionRegistrations.cpp`); retail `/lua/platoon.lua` AttackForceAI; engine `sim_bindings.cpp` IssueFormAttack | Open |
| FAF: `Unit:ForceAltFootprint(bool)` is unbound | FAF: a script error when a human-built Salem finishes on water and at each press of its amphibious toggle; the toggle never confines it to water | FA-Binary-Patches `section/EntityGetFootprint.cpp`; FAF `/units/URS0201/URS0201_script.lua:163-188` | Open |
| FAF: sim `Unit:GetCommandQueue()` gives Moho's command numbers, `x,y,z`, `targetId` and `blueprintId`; the engine gives its own numbers and positions only for Move and Attack | FAF: "copy orders" and "distribute orders" mis-map every order whose number differs (a build reads as Repair, an upgrade as attack-move) and error on capture and later orders | FA-Binary-Patches `section/SimGetCommandQueue.cpp`; FAF `/lua/sim/commands/shared.lua:237`, `copy-queue.lua:72`, `distribute-queue.lua:77-84`; engine `bindings/sim/unit.cpp` GetCommandQueue, `sim/unit_command.hpp` | Open |
| FAF: `SetCommandSource(army, source, bool)` is a no-op | FAF: with the Union army option, orders to an ally's units are dropped; with Common army, the merged players can't command the team army | FA-Binary-Patches `section/SimSetCommandSource.cpp`; FAF `/lua/simInit.lua:461-526`; engine `sim_bindings.cpp` stub_noop, `sim/sim_state.cpp` source armies | Open |
| FAF: sim `SessionIsReplay()` is always false | FAF: in a replay, once the viewer focuses a player, the score sent to the UI holds only that player's and allies' details | FA-Binary-Patches `section/SimIsReplay.cpp`; FAF `/lua/sim/score.lua:288`; engine `sim_bindings.cpp` stub_false | Open |

## UI Lua API

| Missing | Effect | Evidence | Status |
|---|---|---|---|
| `_c_CreateDecal` (`UserDecal`) is bound in retail; the engine doesn't bind it | With an area weapon or a nuke in attack mode, `OnUpdateCursor` errors on every cursor update and no target-area decal follows the cursor | faf-re `_c_CreateDecal` (`script/ScriptedDecal.cpp`); retail `/lua/ui/controls/worldview.lua` (AreaTargetDecal), `/lua/user/UserDecal.lua`; engine `binding_baseline_retail.txt` | Open |
| `RenderOverlayEconomy` gates the world view's per-unit economy readout in retail, and the minimap draws cartographic by default (`SetCartographic`); the engine stores both flags and draws neither | *Seen:* the minimap isn't cartographic, and the Cartographic and economy-overlay toggles do nothing | faf-re `RenderOverlayEconomy` (`sim/SimStartupRegistrations.cpp`), `SetCartographic` (`ui/UiRuntimeTypes.cpp`); retail `/lua/ui/game/minimap.lua`, `/lua/ui/game/gamemain.lua`; engine `renderer.hpp` ("Nothing draws the overlay yet") | Open |
| `TeamColorMode(bool)` is bound in retail and colours units from `GameColors.TeamColorMode` (self, ally, enemy, neutral); the engine doesn't bind it | The team-colour button errors and units keep their army colours. FAF also passes a string of per-army colours and calls it at the start when its option is on | faf-re `TeamColorMode` (`sim/SimStartupRegistrations.cpp`); retail `/lua/ui/game/multifunction.lua`, `GameColors.lua`; FA-Binary-Patches `section/TeamColorMode.cpp` | Open |
| `PostDragger(origin, keycode, dragger)`: retail ends the drag only on the release of the button that started it; the engine ends it on any release | Releasing another mouse button mid-drag ends it: sliders, window moves and resizes, map-marker drags | faf-re `PostDragger` (`ui/UiRuntimeTypes.cpp`); retail `/lua/maui/slider.lua`, `/lua/maui/window.lua`; engine `moho_bindings.cpp` l_PostDragger | Open |
| `ItemList:ShowMouseoverItem` defaults to false in retail, and while it is set the hovered row is drawn in the mouseover colours; the engine defaults it to true and draws no hovered row | No hover highlight in the map-select, profile and combo-box lists | faf-re `CMauiItemList` (`ui/UiRuntimeTypes.cpp`: ShowMouseoverItem); retail `/lua/ui/dialogs/mapselect.lua`, `/lua/ui/controls/combo.lua`; engine `ui_control.hpp` show_mouseover_ | Open |
| `ConTextMatches` and `RemoveProfileDirectories` are bound in retail; the engine binds neither | Typing in the console errors on each key and lists no completions; deleting a profile errors and leaves its saves and replays | faf-re `ConTextMatches` (`console/CConCommand.cpp`), `RemoveProfileDirectories` (`sim/Sim.cpp`); retail `/lua/ui/dialogs/console.lua`, `/lua/ui/dialogs/profile.lua` | Open |
| FAF: world drawing (`WorldView:SetCustomRender`, `OnRenderWorld(dt)`, `UI_DrawRect/Circle/Line`) | FAF: drawing on the map and shape overlays are invisible | FA-Binary-Patches `section/DrawFunc.cpp`, `WorldView.cpp`; FAF `/lua/ui/controls/components/WorldViewShapeComponent.lua:50,68`; engine `bindings/ui/world.cpp` | Open |
| FAF: `CopyToClipboard(str)` | FAF: Ctrl-clicking a chat line errors | FA-Binary-Patches `section/CopyToClipBoard.cpp`; FAF `/lua/ui/game/chat/ChatLinesInterface.lua:77` | Open |
| FAF: `GetDepositsAroundPoint` (UI and sim) is unbound, `GetHighlightCommand()` returns nil | FAF: the context-based templates hotkey errors | FA-Binary-Patches `section/SimGetDepositsAroundPoint.cpp`, `GetHighlightCommand.cpp`; FAF `/lua/ui/game/hotkeys/context-based-templates.lua:301,351` | Open |
| FAF: console variable `ui_StrategicIconScale`, and `cam_DefaultMiniLOD 0` skipping minimap meshes | FAF: the strategic-icon scale option and the minimap's "disable meshes" toggle do nothing | FA-Binary-Patches `section/Icons/Strategic.cxx`, `section/MinimapMesh.cpp`; FAF `/lua/options/options.lua:1051`, `/lua/ui/game/multifunction.lua:481-484` | Open |
| FAF: side mouse buttons `XButton1/2` | FAF: bindings to mouse 4 and 5 don't fire | FA-Binary-Patches `section/OnWindowMessage.cpp`; FAF `/lua/keymap/keyNames.lua:9-10` | Open |

## Blueprint fields

| Missing | Effect | Evidence | Status |
|---|---|---|---|
| Projectile `Physics.LeadTarget` defaults to true in retail; the engine's default is false | Homing projectiles fly at where their target is, not ahead of it: 82 of retail's 102 `TrackTarget` projectiles leave the field unset | faf-re `RProjectileBlueprint` constructor (`LeadTarget(1)`), `Projectile.cpp`; engine `projectile.hpp` lead_target | Open |
| `KLift`, `KLiftDamping`, `LiftFactor` drive an aircraft's climb in retail (KLift divided by its transport load), and slow it (down to 0.04×) when terrain ahead rises more than LiftFactor; the engine climbs at a fixed 5 per second | Aircraft climb and descend at one rate, and don't slow for rising terrain | faf-re `CUnitMotion`, `CAiNavigatorAir`; engine `navigator.cpp` climb_rate_ | Open |
| `PrefersPrimaryWeaponTarget` makes a weapon take the primary's target when it can hit it; `StopOnPrimaryWeaponBusy` makes one drop its target while the primary has one. The engine targets each weapon alone | A unit's other weapons don't share its primary's target: 73 weapons on 36 units (ships, gunships, ASF, Fatboy, Monkeylord); Janus and Notha keep firing their second weapon | faf-re `CAcquireTargetTask`; engine `weapon.cpp` | Open |
| `RaisedPlatforms` raises land units on a factory's deck to its platform quads in retail; the engine snaps them to terrain or water | Units on a factory's deck, new ones rolling off among them, stay at ground level inside the deck (25 retail factories and gates) | faf-re `CUnitMotion` (ground snap); engine `Unit::ground_y` | Open |
| `SelectionYOffset` cuts the top off a unit's pick box by that fraction in retail (default 0.5; ACUs and SACUs 0, transport beacons 1); the engine picks by the whole box | The cursor picks a unit anywhere in its box; retail picks most units only by the lower half of theirs | faf-re `CUIWorldView::UpdateSelection`; engine `InputHandler::unit_under` | Open |
| `SelectionMeshScaleX/Z`, `SelectionMeshUseTopAmount` narrow a unit's pick box in retail | UEF and Cybran naval factories are picked by their whole box; retail picks them only by the top 15%, UEF ones narrowed to 0.3×/0.4× | faf-re `CUIWorldView::UpdateSelection`; engine `InputHandler::unit_under` | Open |
| `NeedToFaceTargetToBuild` turns a builder to its site before it builds in retail | Seraphim engineers (XSL0105, XSL0208, XSL0309) build without turning to the site | faf-re `CUnitMobileBuildTask`, `CUnitRepairTask`; engine `unit_orders.cpp` order_build_mobile | Open |
| `CameraFollowsProjectile`, `CameraFollowTimeout`: retail moves a camera tracking a unit onto such a shot for the timeout | With the camera tracking a unit, its nukes, missiles, shells and bombs (90 retail projectiles) don't take the camera along | faf-re `Projectile.cpp` (constructor), `CameraImpl`; engine: no reader | Open |
| Projectile `StrategicIconSize`: retail draws a projectile without an icon name as a square in its army's colour in the strategic view; the engine draws no projectiles there | The strategic view shows no projectiles (106 retail projectiles set the size) | faf-re `CWldSession` (strategic icons); engine `strategic_icon_renderer.cpp` | Open |
| Projectile `RotationalVelocity`, `RotationalVelocityRange`: retail spins a projectile about a random axis at creation | Four meshed retail projectiles don't spin (Brackman hack pegs, neutron cluster bomblets, Kril torpedo, meson rocket) | faf-re `RProjectileBlueprint`, `Projectile.cpp` (constructor); engine `projectile.hpp` | Open |

## Categories

| Missing | Effect | Evidence | Status |
|---|---|---|---|
| `SELECTABLE`: retail selects only `SELECTABLE` units, and a unit being built only if it is a `FACTORY`; the engine ignores the category and never selects a unit being built | Your own transport beacon (no `SELECTABLE`) can be clicked and boxed; a factory under construction can't be selected | faf-re `UserUnit::IsSelectable` (`unit/core/UserUnit.cpp`), `UserEntity`; engine `input_handler.cpp` selectable() | Open |
| `REBUILDER`: when a structure dies, retail queues its rebuild, same blueprint and spot, on its army's `REBUILDER` units guarding it | Support commanders guarding a structure don't rebuild it | faf-re `CUnitGuardTask`, `Unit::Kill` (`unit/core/Unit.cpp`); engine: no rebuild path | Open |
| `UNTARGETABLE`: retail's cursor passes through a unit that is `UNTARGETABLE` and neither `SELECTABLE` nor `FERRYBEACON`; the engine applies it to props only | Hovering such a unit (URA0001, URB5206, XSC9010/9011, the Othuy XSL0402) shows it, and a right click on an enemy one is an Attack instead of a Move | faf-re `CUIWorldView::UpdateSelection`; engine `input_handler.cpp` targetable_prop, handle_right_click | Open |
| `FAVORSWATER`: retail paths the Salem (URS0201) with its water footprint while it and its destinations are on water; the engine always uses its amphibious footprint | The Salem can take land routes where retail keeps to water | faf-re `CAiPathNavigator`; engine `path_navigator.cpp`, `alt_footprint()` unread | Open |
| `SHOWQUEUE`: retail's queue display is empty for a unit without it; the engine shows any unit's queued builds | Selecting one non-`SHOWQUEUE` builder with queued builds (e.g. the UEF ACU's drone UEA0001) shows a queue retail doesn't | faf-re `UserUnit.cpp` (`SetCurrentFactoryForQueueDisplay`); retail `/lua/ui/game/construction.lua`; engine `factory_queue.cpp` | Open |
| Order validity: retail's sim refuses Reclaim, Capture and Repair from a unit without `RECLAIM`, `CAPTURE`, `REPAIR` (or a docked `POD`), and builds from one that isn't `FACTORY`, `ENGINEER`, `NEEDMOBILEBUILD` or `POD`; the engine decides by the command caps | The UEF SACU's pod UEA0003 (Capture cap, no `CAPTURE`) takes capture orders and captures; retail drops them | faf-re `Sim.cpp` (order filter); retail `/units/UEL0301/UEL0301_script.lua`; engine `sim_state.cpp` takes_command | Open |
| FAF: `CQUEMOV` makes a half-built unit selectable and able to queue orders, as `FACTORY` does in retail | FAF: a half-built mex or point defence can't be selected to queue its upgrade | FA-Binary-Patches `section/SelectUnit.cpp`, `section/BuildUnit.cpp`; FAF `/units/UEB1103/UEB1103_unit.bp` and 94 more | Open |

## Engine callbacks

| Missing | Effect | Evidence | Status |
|---|---|---|---|
| `ShowEscapeDialog`: retail calls it when the window is closed | Closing the window quits at once; retail asks for confirmation | retail `/lua/ui/uimain.lua` ShowEscapeDialog, `/lua/ui/dialogs/eschandler.lua`; engine `app/window.cpp` | Open |
| `OnDecayed`: retail drains a unit under construction from its second tick (0.1 / max(BuildCostEnergy, BuildCostMass, BuildTime) a tick) and calls `OnDecayed` at no health, which destroys it | Abandoned unfinished structures keep their progress and stay for ever | faf-re `Unit::MotionTick` (`unit/core/Unit.cpp`); retail `/lua/sim/Unit.lua` OnDecayed; engine `unit.cpp`: no decay | Open |
| `OnStopBuilderTracking`: retail calls it when a builder arm leaves its target; `ConstructionUnit` folds the arm | UEF engineers (UEL0105/0208/0309) and the Fatboy keep their build arms out after a build | faf-re `CBuilderArmManipulator`; retail `/lua/defaultunits.lua` ConstructionUnit; engine `unit.cpp` aim_builder_arms | Open |
| `ShowDesyncDialog`: retail shows a dialog naming the desynced players | A desync is detected but not shown to the player | retail `/lua/ui/uimain.lua` ShowDesyncDialog, `/lua/ui/dialogs/desync.lua`; engine `gpgnet_session.cpp` report_desync | Open |
| `OnCommandGraphShow`: retail calls it while Shift is held | With Shift held, map-marker pings don't show their name panel and can't be dragged or Ctrl+right-click deleted | faf-re `UICommandGraph`; retail `/lua/ui/game/commandgraph.lua` → `/lua/ui/controls/worldview.lua` ShowPings | Open |
| `OnPlayNoStagingPlatformsVO` / `OnPlayBusyStagingPlatformsVO`: retail's Dock plays a voice when there is no air staging platform or all are full | Dock silently does nothing in those cases | faf-re `IssueDockCommand`; retail `/lua/aibrain.lua` OnPlayNoStagingPlatformsVO; engine `user_bindings.cpp` IssueDockCommand | Open |
| `OnTrackUnit`: retail calls it while the camera tracks a unit | No tracking mode text | retail `/lua/ui/game/tracking.lua` OnTrackUnit; engine `user_bindings.cpp` UI_TrackUnit | Open |

## Behaviour

| Missing | Effect | Evidence | Status |
|---|---|---|---|
| Retail's right click on an enemy is an attack only if a selected unit can hit it (`OVERLAYANTIAIR`, `OVERLAYDIRECTFIRE`, `OVERLAYANTINAVY`, then each weapon's layers and categories); the engine makes it an attack whenever a unit has the Attack cap | Right-clicking an enemy no selected weapon can hit (tanks on a bomber) gives an Attack order | faf-re `UserUnit::CanAttackTarget`, `func_GetRightMouseButtonAction`; engine `input_handler.cpp` | Open |

### Where the engine does retail's way and FAF's exe differs

| Retail behaviour kept | FAF's exe | Evidence | Status |
|---|---|---|---|
| A moving unit's intel is repainted once it has moved a third of its radius, or after 30 ticks | Also after 5 ticks: a slow unit's vision and radar trail it by at most 0.5 s instead of 3 s | faf-re `CIntelPosHandle::UpdatePos`; FA-Binary-Patches `hooks/IntelUpdate.cpp`; engine `intel_sources.hpp` | Open |
| A paused engineer keeps reclaiming | Pausing stops the reclaim | faf-re `CUnitReclaimTask`; FA-Binary-Patches `hooks/StopReclaimWhenPaused.cpp` | Open |
| Double-clicking a wall selects nothing more | Selects the walls of that type on screen | faf-re `HandleDoubleClickSelection`; FA-Binary-Patches `hooks/WallSelection.cpp`; engine `input_handler.cpp` | Open |

## Closed since the audits

| Item | PR |
|---|---|
| Texture scrollers (`AddThreadScroller`, `AddPingPongScroller`, `AddManualScroller`, `RemoveScroller`) and the mesh LOD's `Scrolling`: tank treads (36 retail blueprints with `Treads.ScrollTreads`) and UEA0102's ping-pong scroll move | [#540](https://github.com/bwiemz/supcom-engine/pull/540) |
| `AddBuildRestriction(army, category)` keeps a category's blueprints on the army; `GetUnitCommandData` reads them (campaign and tutorial restrictions) | [#479](https://github.com/bwiemz/supcom-engine/pull/479) |
| `SetArmyColorIndex` and the civilian army's colour | [#457](https://github.com/bwiemz/supcom-engine/pull/457) |
| `ChangeUnitArmy` keeps commanders and units being built | [#471](https://github.com/bwiemz/supcom-engine/pull/471) |
| FAF: `GiveNukeSiloAmmo(blocks, true)` sets the silo's done blocks | [#467](https://github.com/bwiemz/supcom-engine/pull/467) |
| `SimCallback` carries its whole `Args`, nested tables included (pings, self-destruct, ping groups) | [#460](https://github.com/bwiemz/supcom-engine/pull/460) |
| `EnableResourceRendering`: the deposits' strategic icons | [#459](https://github.com/bwiemz/supcom-engine/pull/459) |
| `UserUnit:GetCommandQueue` gives Moho's rows (`type`, `position`) and a factory's rally orders; a factory starts with its initial rally | [#474](https://github.com/bwiemz/supcom-engine/pull/474), [#475](https://github.com/bwiemz/supcom-engine/pull/475) |
| UI `WorldMesh`: `SetMesh`, `SetStance`, `SetHidden`, `SetLifetimeParameter`; the rally-point flag | [#473](https://github.com/bwiemz/supcom-engine/pull/473) |
| Shift+right click on an earlier move's waypoint turns the moves from it into a patrol; `ShowConvertToPatrolCursor` | [#456](https://github.com/bwiemz/supcom-engine/pull/456) |
| `NOSPLASHDAMAGE`: area damage reaches enemy projectiles that have a collision shape (about 40 of retail's, some 23 of them without `NOSPLASHDAMAGE`), and spares `NOSPLASHDAMAGE` ones | [#472](https://github.com/bwiemz/supcom-engine/pull/472) |
| `SUBCOMMANDER`: a support commander in formation keeps out of patrol work | [#470](https://github.com/bwiemz/supcom-engine/pull/470) |
| A build reclaims the props on its site first; a structure rebuilt on its wreck starts at the builder's `GetRebuildBonus` (`RebuildBonusIds`, `AssociatedBP`) | [#469](https://github.com/bwiemz/supcom-engine/pull/469) |
| Repair of a structure under construction runs `OnStartBuild(target, 'Repair')` (assist beams) and aims the arm | [#463](https://github.com/bwiemz/supcom-engine/pull/463) |
| Builders work once their arm is on target | [#468](https://github.com/bwiemz/supcom-engine/pull/468) |
| Engineers assisting a shield generator regenerate its bubble (`ShieldIsOn`, `RegenAssistMult`) | [#464](https://github.com/bwiemz/supcom-engine/pull/464) |
| A patrol's break-offs keep Moho's pace and its leg's search box | [#461](https://github.com/bwiemz/supcom-engine/pull/461) |
| One command graph line per shared order, `CalculateWaypointLineWidth` wide | [#455](https://github.com/bwiemz/supcom-engine/pull/455) |
| An order moved or taken off shows at once, paused or before its tick | [#454](https://github.com/bwiemz/supcom-engine/pull/454) |
| A unit aboard a transport can't be selected | [#458](https://github.com/bwiemz/supcom-engine/pull/458) |
| Orders and their cursor take the unit under the cursor; Capture only on what Moho offers it | [#477](https://github.com/bwiemz/supcom-engine/pull/477) |
| Past zoom 150 the cursor and right-click pass over props | [#478](https://github.com/bwiemz/supcom-engine/pull/478) |
| A press reaches the control the global click handler destroyed (chat recipient) | [#462](https://github.com/bwiemz/supcom-engine/pull/462) |
| Right-button drag moves the selection in formation, facing the drag, with ghosts in their slots | [#465](https://github.com/bwiemz/supcom-engine/pull/465), [#466](https://github.com/bwiemz/supcom-engine/pull/466) |
| Selection brackets as Moho sizes them: `SelectionCenterOffsetX/Y/Z`, `SelectionSizeX/Z`, `SelectionThickness` | [#483](https://github.com/bwiemz/supcom-engine/pull/483) |
| Console variables and range profiles exist before the window (`range_RenderSelected`/`Highlighted`/`Build`, `Cam_Free`, `UI_RenderUnitBars`, `ren_SelectBoxes`) | [#481](https://github.com/bwiemz/supcom-engine/pull/481) |
| Partly: Moho's random stream (MT19937 with `CRandomStream`'s conversions), opt-in behind `--moho-random`; the default stays SplitMix64, and `FRandGaussian`'s cached second value isn't Moho's yet | [#480](https://github.com/bwiemz/supcom-engine/pull/480) |
| Veterancy counts kills (`KILLS`, not BENIGN or unfinished) | [#383](https://github.com/bwiemz/supcom-engine/pull/383) |
| Reclaiming a unit wears it down, then `CreateWreckageProp` and its wreck | [#380](https://github.com/bwiemz/supcom-engine/pull/380) |
| `SelectUnits(nil)` clears the selection | [#376](https://github.com/bwiemz/supcom-engine/pull/376) |
| `RECLAIMABLE` in the order and the pick | [#382](https://github.com/bwiemz/supcom-engine/pull/382) |
| `BENIGN` not picked by weapons' own targeting | [#377](https://github.com/bwiemz/supcom-engine/pull/377) |
| `Bitmap:UseAlphaHitTest` | [#381](https://github.com/bwiemz/supcom-engine/pull/381) |
| `AcquireKeyboardFocus(false)` | [#378](https://github.com/bwiemz/supcom-engine/pull/378) |
| `AttachBoneToEntityBone` argument order (death debris) | [#379](https://github.com/bwiemz/supcom-engine/pull/379) |
| `OnAnimCollision`, `OnAnimTerrainCollision` (footfalls, crashes) | [#390](https://github.com/bwiemz/supcom-engine/pull/390), [#400](https://github.com/bwiemz/supcom-engine/pull/400) |
| Build templates (`Generate/Get/Set/ClearBuildTemplates`, `GenerateBuildTemplateFromSelection`), placed whole | [#410](https://github.com/bwiemz/supcom-engine/pull/410), [#436](https://github.com/bwiemz/supcom-engine/pull/436) |
| `AutoLandTime` | [#419](https://github.com/bwiemz/supcom-engine/pull/419) |
| `Circling*`, `HoverOverAttack`, `BankFactor`, `CirclingDirChange`, `TARGETCHASER` | [#422](https://github.com/bwiemz/supcom-engine/pull/422), [#431](https://github.com/bwiemz/supcom-engine/pull/431) |
| `OccupancyCaps`, `OccupyRects`, `MaxSlope` (pathing by them behind `--moho-pathing`), `SpecFootprints` | [#406](https://github.com/bwiemz/supcom-engine/pull/406), [#398](https://github.com/bwiemz/supcom-engine/pull/398) |
| `TargetBones` | [#397](https://github.com/bwiemz/supcom-engine/pull/397) |
| `UseFiringSolutionInsteadOfAimBone`, `MuzzleVelocityRandom` | [#405](https://github.com/bwiemz/supcom-engine/pull/405) |
| Projectile lead (with `LeadTarget` set), `MaxZigZag`, `ZigZagFrequency`, `StartTurnDistance` | [#408](https://github.com/bwiemz/supcom-engine/pull/408) |
| `SetOverlayFilter`, `EffectiveRadius` | [#407](https://github.com/bwiemz/supcom-engine/pull/407) |
| `HasHighlightCommand`, `SetHighlightEnabled` | [#385](https://github.com/bwiemz/supcom-engine/pull/385) |
| Console commands `UI_TrackUnit`, `UI_ExpandCurrentSelection`, `UI_ShowRenameDialog`, `RenameUnit`, `cam_Free`; `ShowRenameDialog` | [#413](https://github.com/bwiemz/supcom-engine/pull/413) |
| `OnKeySelect`, `OnKeyboardFocusChange` | [#391](https://github.com/bwiemz/supcom-engine/pull/391) |
| `OnCommandDragBegin/End` | [#388](https://github.com/bwiemz/supcom-engine/pull/388) |
| `Control:HitTest`, `Physics.CollideEntity` | [#363](https://github.com/bwiemz/supcom-engine/pull/363) |
| `OnCollisionCheck` asked of the struck entity only | [#368](https://github.com/bwiemz/supcom-engine/pull/368) |
| `CannotAttackGround` (ground attack) | [#364](https://github.com/bwiemz/supcom-engine/pull/364) |
| `SlavedToBody`, `SlavedToBodyArcRange`, `AttackAngle`, `TurnFacingRate`, `RotateBodyWhileMoving` | [#367](https://github.com/bwiemz/supcom-engine/pull/367) |
| `AddCommandFeedbackBlip` | [#365](https://github.com/bwiemz/supcom-engine/pull/365) |
| `OnDoubleClick` | [#370](https://github.com/bwiemz/supcom-engine/pull/370) |
| `LayerChangeOffsetHeight` | [#366](https://github.com/bwiemz/supcom-engine/pull/366) |
| `MATH_Lerp`'s five arguments in the sim; `OnHide`; `HideLifebars`, `LifeBarRender` | [#362](https://github.com/bwiemz/supcom-engine/pull/362) |
| `YawOnlyOnTarget` | [#360](https://github.com/bwiemz/supcom-engine/pull/360) |
| `AI.NeedUnpack` | [#354](https://github.com/bwiemz/supcom-engine/pull/354) |
| `AutoInitiateAttackCommand` | [#350](https://github.com/bwiemz/supcom-engine/pull/350) |
| `Winged`, `BreakOffIfNearNewTarget`, `RealisticOrdinance` | [#349](https://github.com/bwiemz/supcom-engine/pull/349) |
| `NotifyUpgrade` | [#348](https://github.com/bwiemz/supcom-engine/pull/348) |
| `AI.InitialAutoMode` (silos build on their own) | [#347](https://github.com/bwiemz/supcom-engine/pull/347) |
| Weapon `LeadTarget`, `AlwaysRecheckTarget` default true | [#339](https://github.com/bwiemz/supcom-engine/pull/339) |
| `PATROLHELPER`; `RECLAIMABLE`/`BENIGN` on patrol and guard | [#335](https://github.com/bwiemz/supcom-engine/pull/335), [#356](https://github.com/bwiemz/supcom-engine/pull/356) |
| `SetCaretCycle` | [#334](https://github.com/bwiemz/supcom-engine/pull/334) |
| `OnEnterPressed`, `OnEscPressed` returns | [#328](https://github.com/bwiemz/supcom-engine/pull/328) |
| `IN_ClearKeyMap` | [#327](https://github.com/bwiemz/supcom-engine/pull/327) |
| `DisableHitTest(recursive)` | [#326](https://github.com/bwiemz/supcom-engine/pull/326) |
| `UNTARGETABLE` on props | [#325](https://github.com/bwiemz/supcom-engine/pull/325) |

Not gaps, checked: `CanWeaponFire`, `TaskTick` returning nil, `VerifyScriptCommand`, `IsScrollable`, `SetIgnoreArmyCap`, `GetTerrainTypeOffset`, `INSIGNIFICANTUNIT`, `HYDROCARBON`, `ReTargetOnMiss`, `RecoilImpulse` (no retail blueprint sets `ShipRock`). No difference on retail's data: `OnDamageBy` and `OnNukeArmed` (no retail blueprint has the voice keys they read), `GetCaptureCosts` (the engine's formula is retail `Unit.lua`'s, and nothing overrides it), `NeedPrep`, `DetachFrom`'s flag (every retail call passes true), `IssueFormPatrol` (no shipped map reaches its caller).
