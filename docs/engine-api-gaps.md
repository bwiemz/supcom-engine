# Engine API gaps

Engine API that retail's Lua and data use and the engine doesn't support, or supports in part, with what FAF adds on top. Found by audits of the bindings' optional and flag arguments, the callbacks' return values, the data's flags and categories, the parity-ratchet baselines and FAF's executable patches, and by play. Checked against main at 6570e4c6 (2026-10-09).

How to read it:
- Each group lists retail's gaps first, ranked by how visible they are in play, then FAF's additions.
- **Missing** gives retail's rule and what the engine does instead. faf-re is decompiled from FAF's patched executable; each rule cited from it was checked against retail's, and rows that follow FAF's patches say so. faf-re paths are relative to `src/sdk/moho`.
- **Effect** follows the path from retail's Lua through the engine's code to what the player sees; *seen* marks gaps also observed in play on this engine.
- **Status:** *Open*, an open PR, or the PR that closed it. Closed items are listed at the end.
- The ratchet baselines (`tools/*_baseline.txt`, `tests/integration/binding_baseline_retail.txt`) list every unread field, stub and unmade callback, and `docs/plans/2026-10-05-parity-ratchet-triage.md` rates them all. Only those with an effect traced here are repeated.

## Sim Lua API

| Missing | Effect | Evidence | Status |
|---|---|---|---|
| `FlattenMapRect(x, z, sx, sz, y)` writes the heightfield in retail; the engine's is a no-op | Terrain under structures with a FlattenSkirt (219 retail blueprints) isn't levelled; buildings stand on the slope | faf-re `cfunc_FlattenMapRect` (`sim/Sim.cpp`); retail `/lua/defaultunits.lua` OnCreate; engine `sim_bindings.cpp` stub_noop | Open |
| Scrollers (`AddThreadScroller`, `AddPingPongScroller`, `AddManualScroller`, `RemoveScroller`) are no-ops; the renderer ignores the mesh LOD's `Scrolling` | Tank tracks don't move (37 blueprints with `Treads.ScrollTreads`); UEA0102's ping-pong scroll doesn't run | faf-re `AddThreadScroller` (`entity/Entity.cpp`); retail `/lua/sim/Unit.lua` CreateTreads, `/lua/defaultunits.lua`; engine `entity.cpp` stub_noop | Open |
| Retail calls a prop's `OnCollision` when a unit runs into it; the engine has no unit-prop collision | Tanks driving through a forest don't knock trees over (`proptree.lua` OnCollision → FallDown → Whack) | faf-re `CUnitMotion::ProcessSurfaceCollisionFromLastMove` (`unit/CUnitMotion.cpp`: after each surface unit's move, every fifth tick for one whose SizeX × SizeZ is over 0.2, props in its box), `Sim::DoCollisionsFor` (`sim/Sim.cpp`); retail `/lua/proptree.lua`; engine has no such pass | Open |
| `CreateThrustController`'s manipulator swivels engine bones with thrust in retail (`SetThrustingParam`'s 8 parameters); the engine's never turns them | UEF transports and gunships (UEA0104, UEA0107, UEA0203, UEA0305, XEA0306): nozzles stay in their animation pose | faf-re `CThrustManipulator` (`animation/CThrustManipulator.cpp`); retail `/units/UEA0107/UEA0107_Script.lua`; engine `manipulator.hpp` (ThrustManipulator::tick empty) | Open |
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
| FAF: world drawing (`WorldView:SetCustomRender`, `OnRenderWorld(dt)`, `UI_DrawRect/Circle/Line`) | FAF: drawing on the map and shape overlays are invisible | FA-Binary-Patches `section/DrawFunc.cpp`, `WorldView.cpp`; FAF `/lua/ui/controls/components/WorldViewShapeComponent.lua:50,68`; engine `bindings/ui/world.cpp` | Open |
| FAF: `CopyToClipboard(str)` | FAF: Ctrl-clicking a chat line errors | FA-Binary-Patches `section/CopyToClipBoard.cpp`; FAF `/lua/ui/game/chat/ChatLinesInterface.lua:77` | Open |
| FAF: `GetDepositsAroundPoint` (UI and sim) is unbound, `GetHighlightCommand()` returns nil | FAF: the context-based templates hotkey errors | FA-Binary-Patches `section/SimGetDepositsAroundPoint.cpp`, `GetHighlightCommand.cpp`; FAF `/lua/ui/game/hotkeys/context-based-templates.lua:301,351` | Open |
| FAF: console variable `ui_StrategicIconScale`, and `cam_DefaultMiniLOD 0` skipping minimap meshes | FAF: the strategic-icon scale option and the minimap's "disable meshes" toggle do nothing | FA-Binary-Patches `section/Icons/Strategic.cxx`, `section/MinimapMesh.cpp`; FAF `/lua/options/options.lua:1051`, `/lua/ui/game/multifunction.lua:481-484` | Open |
| FAF: side mouse buttons `XButton1/2` | FAF: bindings to mouse 4 and 5 don't fire | FA-Binary-Patches `section/OnWindowMessage.cpp`; FAF `/lua/keymap/keyNames.lua:9-10` | Open |

## Blueprint fields

| Missing | Effect | Evidence | Status |
|---|---|---|---|
| Projectile `Physics.LeadTarget` defaults to true in retail; the engine's default is false | Homing projectiles fly at where their target is, not ahead of it: 82 of retail's 102 `TrackTarget` projectiles leave the field unset | faf-re `RProjectileBlueprint` constructor (`LeadTarget(1)`), `Projectile.cpp`; engine `projectile.hpp` lead_target | Open |
| `PrefersPrimaryWeaponTarget` makes a weapon take the primary's target when it can hit it; `StopOnPrimaryWeaponBusy` makes one drop its target while the primary has one. The engine targets each weapon alone | A unit's other weapons don't share its primary's target: 73 weapons on 36 units (ships, gunships, ASF, Fatboy, Monkeylord); Janus and Notha keep firing their second weapon | faf-re `CAcquireTargetTask`; engine `weapon.cpp` | Open |
| `CameraFollowsProjectile`, `CameraFollowTimeout`: retail moves a camera tracking a unit onto such a shot for the timeout | With the camera tracking a unit, its nukes, missiles, shells and bombs (90 retail projectiles) don't take the camera along | faf-re `Projectile.cpp` (constructor), `CameraImpl`; engine: no reader | Open |

## Categories

| Missing | Effect | Evidence | Status |
|---|---|---|---|
| `SELECTABLE`: retail selects only `SELECTABLE` units, and a unit being built only if it is a `FACTORY`; the engine ignores the category and never selects a unit being built | Your own transport beacon (no `SELECTABLE`) can be clicked and boxed; a factory under construction can't be selected | faf-re `UserUnit::IsSelectable` (`unit/core/UserUnit.cpp`), `UserEntity`; engine `input_handler.cpp` selectable() | Open |
| `REBUILDER`: when a structure dies, retail queues its rebuild, same blueprint and spot, on its army's `REBUILDER` units guarding it | Support commanders guarding a structure don't rebuild it | faf-re `CUnitGuardTask`, `Unit::Kill` (`unit/core/Unit.cpp`); engine: no rebuild path | Open |
| `UNTARGETABLE`: retail's cursor passes through a unit that is `UNTARGETABLE` and neither `SELECTABLE` nor `FERRYBEACON`; the engine applies it to props only | Hovering such a unit (URA0001, URB5206, XSC9010/9011, the Othuy XSL0402) shows it, and a right click on an enemy one is an Attack instead of a Move | faf-re `CUIWorldView::UpdateSelection`; engine `input_handler.cpp` targetable_prop, handle_right_click | Open |
| `FAVORSWATER`: retail paths the Salem (URS0201) with its water footprint while it and its destinations are on water; the engine always uses its amphibious footprint | The Salem can take land routes where retail keeps to water | faf-re `CAiPathNavigator`; engine `path_navigator.cpp`, `alt_footprint()` unread | Open |

## Engine callbacks

| Missing | Effect | Evidence | Status |
|---|---|---|---|

## Behaviour

| Missing | Effect | Evidence | Status |
|---|---|---|---|

### Where the engine does retail's way and FAF's exe differs

| Retail behaviour kept | FAF's exe | Evidence | Status |
|---|---|---|---|
| A moving unit's intel is repainted once it has moved a third of its radius, or after 30 ticks | Also after 5 ticks: a slow unit's vision and radar trail it by at most 0.5 s instead of 3 s | faf-re `CIntelPosHandle::UpdatePos`; FA-Binary-Patches `hooks/IntelUpdate.cpp`; engine `intel_sources.hpp` | Open |
| Double-clicking a wall selects nothing more | Selects the walls of that type on screen | faf-re `HandleDoubleClickSelection`; FA-Binary-Patches `hooks/WallSelection.cpp`; engine `input_handler.cpp` | Open |

## Closed since the audits

| Item | PR |
|---|---|
| `GetBoneDirection(bone)` returns three numbers, not a vector table: retail's Lua destructures `v.x, v.y, v.z = GetBoneDirection(b)`, so the Scathis's `CreateProjectileAtMuzzle` errored in `GetAngleInBetween` and the weapon never fired | pending |
| A right click on an enemy is an attack only if a selected unit can hit it (`UserUnit:CanAttackTarget`); on one none can, the cursor is `RULEUCC_Invalid` and no order is given (tanks on a bomber) | [#531](https://github.com/bwiemz/supcom-engine/pull/531) |
| Projectile `RotationalVelocity`, `RotationalVelocityRange`: a projectile spins about a random axis from its creation (death debris, cluster bomblets, Kril torpedo) | [#532](https://github.com/bwiemz/supcom-engine/pull/532) |
| `brain:GetUnitsAroundPoint(cat, pos, r)` and `GetNumUnitsAroundPoint` with no alliance take any army's units the brain knows of (its own, or one it holds a blip of); with one, only the known units of that alliance (the Ythotha's energy being) | [#533](https://github.com/bwiemz/supcom-engine/pull/533) |
| `brain:GetListOfUnits(cat, needToBeIdle, requireBuilt)`: `needToBeIdle` leaves out units with orders, and `requireBuilt`, when not given, is true (retail's `OnDefeat` no longer kills unfinished units; FAF's wrapper passes false) | [#534](https://github.com/bwiemz/supcom-engine/pull/534) |
| Platoon orders take their squad argument (`MoveToLocation`, `MoveToTarget`, `AggressiveMoveToLocation`, `Patrol`, `AttackTarget`, `GuardTarget`, `Stop`); with none they go to squads Attack to Scout (`AttackTarget`, `GuardTarget`: Attack and Artillery), and with a formation override to the whole platoon (campaign base transports' routes go to the transports only) | [#535](https://github.com/bwiemz/supcom-engine/pull/535) |
| `CombatTurnSpeed`, `TightTurnMultiplier`, `MinAirspeed`, `CirclingTurnMult`: attack runs and circling drive the same attitude controller as ordinary flight, toward the velocity ComputeAirCombatTactics or CalcCirclingOrientation asks; a fighter or bomber banks by its BankFactor in a run, ten times over in a NormalTurn, and chases a moving air target at its distance a second, at least MinAirspeed | [#562](https://github.com/bwiemz/supcom-engine/pull/562) |
| `KTurn`, `KTurnDamping`, `KRoll`, `KRollDamping`, `BankFactor`, `BankForward`: an aircraft turns and rolls under KTurn and KRoll against their dampings toward the attitude CalcWingedOrientation or CalcHoverOrientation asks, its turn error held to TurnSpeed; a winged one banks into its turn by BankFactor, a hovering one leans into its change of velocity (forward too only with BankForward); near its goal it faces the way its order sent it, stopped it holds on the arc its turn would carry it, and it flies at its height to a landing place more than StartTurnDistance away | [#546](https://github.com/bwiemz/supcom-engine/pull/546) |
| `KMove`, `KMoveDamping`: an aircraft's level velocity answers KMove toward the lesser of its distance and its top speed against CalcAirMovementDampingFactor, so it brakes onto the centre of its goal's cell, and with no move to follow holds where its velocity would carry it in a second; it flies through at speed only with another move, attack, patrol or guard order queued (UpdateSpeedThroughStatus), and lands under the same controller | [#544](https://github.com/bwiemz/supcom-engine/pull/544) |
| `KLift`, `KLiftDamping`, `LiftFactor`: an aircraft climbs under KLift over its transport load, against KLiftDamping, toward the highest ground ahead tracked at LiftFactor a second, and slows for ground rising more than LiftFactor | [#537](https://github.com/bwiemz/supcom-engine/pull/537) |
| A paused unit moves, reclaims and fights; a paused builder, repairer or factory keeps its order and state but does no work, and starts nothing new until a retry after it unpauses; a frame whose only builder is a paused engineer decays (FAF's exe also stops a paused unit's prop reclaim) | [#557](https://github.com/bwiemz/supcom-engine/pull/557) |
| An unfinished unit no builder works on decays from its second tick, by 0.1 / max(BuildCostEnergy, BuildCostMass, BuildTime) a tick, and calls `OnDecayed` at no health | [#538](https://github.com/bwiemz/supcom-engine/pull/538) |
| A builder arm calls `OnStartBuilderTracking` and `OnStopBuilderTracking` as its heading starts and stops turning, and turns back to rest at once at a quarter of its slew; `ConstructionUnit` folds the arm after a build | [#539](https://github.com/bwiemz/supcom-engine/pull/539) |
| Order validity: Reclaim, Capture and Repair go only to a unit of `RECLAIM`, `CAPTURE`, `REPAIR` (not a docked `POD`); builds only to a `FACTORY`, `ENGINEER`, `NEEDMOBILEBUILD` or `POD` (the UEF support commander's pod no longer captures) | [#542](https://github.com/bwiemz/supcom-engine/pull/542) |
| FAF: `CQUEMOV` makes a half-built unit selectable and able to take orders, as `FACTORY` does (a half-built mex takes its upgrade and starts it once finished) | [#543](https://github.com/bwiemz/supcom-engine/pull/543) |
| `NeedToFaceTargetToBuild` turns a builder to its site or repair target before it builds (retail's Seraphim engineers) | [#550](https://github.com/bwiemz/supcom-engine/pull/550) |
| `CreateAnimator(unit, true)`: a walk cycle plays at the unit's speed over its MaxSpeed, at least a quarter while it turns | [#551](https://github.com/bwiemz/supcom-engine/pull/551) |
| `MotorFallDown:Whack` tips a tree over tick by tick (a whack that doesn't break it sways it back) | [#553](https://github.com/bwiemz/supcom-engine/pull/553) |
| `SHOWQUEUE`: `SetCurrentFactoryForQueueDisplay` shows the queue only of a unit with the category, and gives `nil` for an empty one | [#555](https://github.com/bwiemz/supcom-engine/pull/555) |
| `RequestRefreshUI()` on a selected unit reports the selection to `OnSelectionChanged` again (enhancements, adjacency, transport loads) | [#556](https://github.com/bwiemz/supcom-engine/pull/556) |
| `Prop:AddBoundedProp(priority)` caps wrecks at 1000, destroying the cheapest, oldest one | [#560](https://github.com/bwiemz/supcom-engine/pull/560) |
| Projectile `StrategicIconSize`: a projectile without an icon name is a square in the strategic view (yellow, as retail's `UI_forceWeaponsToYellow` defaults) | [#561](https://github.com/bwiemz/supcom-engine/pull/561) |
| `RaisedPlatforms`: a unit on a factory's or gateway's deck stands on its platform quads, and rolls off down its ramps | [#564](https://github.com/bwiemz/supcom-engine/pull/564) |
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
| The cursor picks a unit by its mesh's bounds, cut by `SelectionYOffset` and narrowed by `SelectionMeshScaleX/Z`, `SelectionMeshUseTopAmount` | [#525](https://github.com/bwiemz/supcom-engine/pull/525) |
| A drag box takes a unit whose mesh box it meets, not one whose origin it holds; without Shift that box is scaled by `SelectionMeshScaleX/Y/Z` | [#552](https://github.com/bwiemz/supcom-engine/pull/552) |
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
| `PostDragger(origin, keycode, dragger)`: a drag ends only on the release of the button that started it (sliders, window moves and resizes, map-marker drags) | [#545](https://github.com/bwiemz/supcom-engine/pull/545) |
| `ItemList:ShowMouseoverItem` defaults to false; while set, the hovered row is drawn in the mouseover colours (map-select, profile and combo-box lists) | [#545](https://github.com/bwiemz/supcom-engine/pull/545) |
| `ConTextMatches` (the console's completions) and `RemoveProfileDirectories` (a deleted profile's replays and saves) | [#545](https://github.com/bwiemz/supcom-engine/pull/545) |
| `ShowEscapeDialog`: closing the window asks uimain's `ShowEscapeDialog(true)` ("Are you sure you'd like to quit?") instead of quitting at once | [#547](https://github.com/bwiemz/supcom-engine/pull/547) |
| `ShowDesyncDialog(beat, names)`: a desync shows retail's dialog naming the players whose checksums differed | [#547](https://github.com/bwiemz/supcom-engine/pull/547) |
| `OnCommandGraphShow`: Shift held shows the map-marker pings' name panels, drag and Ctrl+right-click delete | [#547](https://github.com/bwiemz/supcom-engine/pull/547) |
| `OnPlayNoStagingPlatformsVO` / `OnPlayBusyStagingPlatformsVO`: Dock with no air staging platform, or none free, plays the brain's voice | [#547](https://github.com/bwiemz/supcom-engine/pull/547) |
| `OnTrackUnit`: the world camera tells `tracking.lua` as it starts and stops following a unit (the "Tracking" mode text) | [#547](https://github.com/bwiemz/supcom-engine/pull/547) |

Not gaps, checked: `CanWeaponFire`, `TaskTick` returning nil, `VerifyScriptCommand`, `IsScrollable`, `SetIgnoreArmyCap`, `GetTerrainTypeOffset`, `INSIGNIFICANTUNIT`, `HYDROCARBON`, `ReTargetOnMiss`, `RecoilImpulse` (no retail blueprint sets `ShipRock`). No difference on retail's data: `OnDamageBy` and `OnNukeArmed` (no retail blueprint has the voice keys they read), `GetCaptureCosts` (the engine's formula is retail `Unit.lua`'s, and nothing overrides it), `NeedPrep`, `DetachFrom`'s flag (every retail call passes true), `IssueFormPatrol` (no shipped map reaches its caller).
