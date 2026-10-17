# Engine API gaps

Engine API that retail's Lua and data use and the engine doesn't support, or supports in part, with what FAF adds on top. Found by audits of the bindings' optional and flag arguments, the callbacks' return values, the data's flags and categories, the parity-ratchet baselines and FAF's executable patches, and by play. Checked against main at 10e85994 (2026-10-12).

How to read it:
- Each group lists retail's gaps first, ranked by how visible they are in play, then FAF's additions.
- **Missing** gives retail's rule and what the engine does instead. faf-re is decompiled from FAF's patched executable; each rule cited from it was checked against retail's, and rows that follow FAF's patches say so. faf-re paths are relative to `src/sdk/moho`.
- **Effect** follows the path from retail's Lua through the engine's code to what the player sees; *seen* marks gaps also observed in play on this engine.
- **Status:** *Open*, an open PR, or the PR that closed it. Closed items are listed at the end.
- The ratchet baselines (`tools/*_baseline.txt`, `tests/integration/binding_baseline_retail.txt`) list every unread field, stub and unmade callback, and `docs/plans/2026-10-05-parity-ratchet-triage.md` rates them all. Only those with an effect traced here are repeated.

## Sim Lua API

| Missing | Effect | Evidence | Status |
|---|---|---|---|

## UI Lua API

| Missing | Effect | Evidence | Status |
|---|---|---|---|
| The minimap draws cartographic by default in retail (`SetCartographic(true)`): Moho's `Cartographic` paints the terrain's elevation as a hypsometric colour ramp with topographic contour bands, then the range rings, fog of war and playable boundary, the meshes in their materials' `cartographicTechnique`, and the map's cartographic decals; the engine stores the flag and draws the minimap as any other view | *Seen:* the minimap isn't cartographic, and the Cartographic toggle does nothing | faf-re `Cartographic` (`render/Cartographic.cpp`), `SetCartographic` (`ui/UiRuntimeTypes.cpp`); retail `/lua/ui/game/minimap.lua`, `/lua/ui/game/gamemain.lua`, `/lua/ui/game/multifunction.lua`; the port's design: `docs/plans/2026-10-13-cartographic-view-design.md` | Part: the map's bands, colours and decal batches are read, and the lookup data computed (#636); the frame drawn offscreen and checked against cartographic.fx (#638); on the minimap (#644); the range rings and fog of war drawn into its frame as Moho's are ([#670](https://github.com/bwiemz/supcom-engine/pull/670)); a main view's (P7, [#677](https://github.com/bwiemz/supcom-engine/pull/677)): its camera orthographic, the map's frame through it in place of the world, the icons on at every zoom, the light particles alone; its meshes in their materials' cartographic techniques (P6, [#678](https://github.com/bwiemz/supcom-engine/pull/678)); the playable boundary in the frames and the world ([#681](https://github.com/bwiemz/supcom-engine/pull/681)); the minimap's meshes, through its own camera's LOD ([#718](https://github.com/bwiemz/supcom-engine/pull/718)) |
| FAF: `__EngineStats` holds only the cursor's `Position`, `Elevation` and `OCell`; Moho fills a tree of the engine's statistics each frame (the camera's `FocusDistance` and the cursor's `LODMetric`, cells, vertices, object counts) | FAF: the engine-statistics debug window and the performance log show next to nothing | FAF `/lua/system/performance.lua:1-90` (a dump of Moho's tree), `/lua/debug/EngineStats.lua`; engine `user_bindings.cpp` update_engine_stats | Open |

## Blueprint fields

| Missing | Effect | Evidence | Status |
|---|---|---|---|

## Categories

| Missing | Effect | Evidence | Status |
|---|---|---|---|
| `REBUILDER`: when a structure dies, Moho queues its rebuild, same blueprint and spot, on its army's `REBUILDER` units whose Guard names that blueprint at that spot. A `REBUILDER`'s Guard takes up no live unit and outlives its target. | The Guard part is ported: a support commander guarding a factory doesn't build with it, and its Guard of a destroyed structure stays, keeping it where the structure stood. The rebuild isn't. The original rebuilds nothing for a Guard from `IssueGuard`, which carries no blueprint (faf-re `cfunc_IssueGuardL`). Whether a player's right-click Guard carries one is not yet known. | faf-re `CUnitGuardTask`, `Unit::LookForStructureRebuilder` (`unit/core/Unit.cpp`); `tools/moho_probes` `rebuild`, `scu-assist` (2026-10-13) | Part: the Guard ported (2026-10-13); the rebuild open |

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
| FAF: `cam_DefaultMiniLOD 0` skips the minimap's meshes, as FAF's patched game does (FA-Binary-Patches `section/MinimapMesh.cpp`), so FAF's "disable meshes" toggle and `minimap.lua` work. The meshes are drawn as retail's `Cartographic::RenderMeshes` draws them: through the minimap's own camera, at the LOD scale `cam_DefaultMiniLOD` held when the view was made (1.8) | [#718](https://github.com/bwiemz/supcom-engine/pull/718) |
| The map's border: `MapBorderAdd`'s meshes, which retail's `UpdateWorldBorderState` asks for with the `world_border` option on (its default), are drawn as Moho's `MapImager` draws them. Each sits over the heightfield's centre at its heights' midpoint plus the map's imager elevation offset, scaled by the map's width times the mesh blueprint's `UniformScale`. It is lit by `MapImagerPS0` and glows by `MapImagerPS1`, and it draws at every zoom, strategic included (faf-re `render/MapImager.cpp`; retail `mesh.fx`). The engine kept the list and drew nothing, so past the map's edge was black | [#685](https://github.com/bwiemz/supcom-engine/pull/685) |
| FAF: `ChangeUnitArmy` gives a commander or a unit being built (its exe's `hooks/HTransferACUs.cpp`), on FAF's data only; FAF's common army's `MergeArmyPair` hands over the merged player's commander | [#684](https://github.com/bwiemz/supcom-engine/pull/684) |
| Command sources (Moho's per-army `mValidCommandSources`): FAF's `SetCommandSource`, `GetCurrentCommandSource`, `OkayToMessWithArmy`, `CheatsEnabled`, FAF's 0-based sim `SetFocusArmy`, the UI's focus checks and console `SetFocusArmy`, and a dropped player's `AbandonedByPlayer` | [#679](https://github.com/bwiemz/supcom-engine/pull/679) |
| `Warp(unit, position)` puts the unit on the layer and at the height a unit made there gets (Moho's `Unit::Warp`: `GetStartingLayer` with none asked for, then `CalcSpawnElevation`), and aborts its move, so the Move order running ends. A tank warped into the sea stands on the seabed and drowns; a frigate floats; a scout flies 18 over the ground; a CZAR lands, then lifts off. The engine kept the height given and the old layer, and a warped mover went on to its goal. The original game was probed for each (tools/moho_probes `warp`) | [#672](https://github.com/bwiemz/supcom-engine/pull/672) |
| FAF: world drawing. With `WorldView:SetCustomRender(true)` the main world view's `OnRenderWorld(self, delta)` runs each frame, and `UI_DrawLine`, `UI_DrawCircle` (48 segments) and `UI_DrawRect` (Moho's `DRAW_Rect` border) draw flat in the world, over it, at least two pixels thick (FA-Binary-Patches `DrawFunc.cpp`, `HDraw.cpp`); outside `OnRenderWorld` they raise an error. FAF's `WorldViewShapeComponent` shapes show. Other views (the minimap) don't call it yet | [#675](https://github.com/bwiemz/supcom-engine/pull/675) |
| `IssueBuildMobile(units, position, blueprint, cells)` gives the build to the one unit that can build it nearest the site (3-D distance), and to none if no unit can (faf-re `cfunc_IssueBuildMobileL`); the engine gave it to every unit. Retail's scripts never call it; FAF's do, and its fifth argument, true, gives it to every unit, as FAF's `IssueBuildAllMobile` asks | [#658](https://github.com/bwiemz/supcom-engine/pull/658) |
| FAF: `UserUnit:GetFractionComplete()`, how far the unit is built and nothing for a unit gone, which FAF's construction and orders panels ask of an external factory unguarded | [#658](https://github.com/bwiemz/supcom-engine/pull/658) |
| FAF: the console variables `ui_StrategicIconScale`, `ui_ProjectileIconScale` and `ui_DepositIconScale` (1, as retail draws them) scale the icons' half sizes as FAF's patched game does: a unit's icon, underlay and stunned badge; a projectile's icon and square; a deposit's icon, in the world and on the minimap | [#658](https://github.com/bwiemz/supcom-engine/pull/658) |
| FAF: a side mouse button is the key `XButton1` or `XButton2` (codes 5 and 6), as FAF's patched game reports them, where the key names name it (FAF's `keyNames.lua` does, retail's doesn't); with retail's names the buttons do nothing, as in retail's game, where the engine had given them as button presses | [#658](https://github.com/bwiemz/supcom-engine/pull/658) |
| A unit works at its consumption request's last grant, as Moho's `ResourceConsumed`: a request made in one tick is granted in the next and worked from the one after, `GetResourceConsumed` answers 0 for a unit asking for nothing, and a request that stops before working its grant gives it back to the army (`SetConsumptionActive(false)`). The engine worked a new request at once and answered 1. A factory's timeline now matches the original game's tick for tick (the timing probe): the unit at once, progress from the third tick, done two ticks later than the engine had it | [#642](https://github.com/bwiemz/supcom-engine/pull/642) |
| The economy grants each request a share of what it asks, as Moho's `ProcessArmyEconomy` does: requests wanting both energy and mass share one ratio, bounded by the scarcer resource, and single-resource requests share what is left. A build starved of energy took all the mass it asked for; in the original game's probe (a factory stalled to 7% by a Monkeylord's upkeep) the engine's mass now rises 17.1 over the stall against the original's 17.2, where it fell 101 | [#640](https://github.com/bwiemz/supcom-engine/pull/640) |
| An army has no storage of its own: the engine gave each 200 energy and 200 mass more than its units hold, so every army started with 4200 and 850 where the original's start with their commander's 4000 and 650. `GiveStorage(type, amount)` replaces the army's one extra storage pair, the amount in its resource and none in the other (`cfunc_CAiBrainGiveStorageL`), as the original game showed: retail's X1CA_006 gives Fletcher and the Seraphim 10000 extra energy storage and no extra mass | [#639](https://github.com/bwiemz/supcom-engine/pull/639) |
| FAF: an order's `commandType` is Moho's `UNITCOMMAND` number in the sim's and the UI's `GetCommandQueue` (a build 8, a repair 20; a formation's move 4, attack 11, attack-move 36; an unload of chosen cargo 25), and the sim's entries give every order's `x, y, z` (a target's where it is), its `target` entity, `targetId` and `blueprintId`, as FAF's `SimGetCommandQueue` patch: copy and distribute orders re-issue the right orders. Numbers of orders the engine doesn't have are never given: a build helped reads Repair (20), not BuildAssist (9); a formation patrol 16, not 18; a guard of a commander 15, not AssistCommander (29), which FAF's UEF pod scripts compare. A target's place is where it is now; what Moho's command holds for one isn't known | [#635](https://github.com/bwiemz/supcom-engine/pull/635) |
| FAF: the sim's `SessionIsReplay()` is true in a replay (`SimIsReplay`), so a replay's viewer focused on a player sees every army's score | [#635](https://github.com/bwiemz/supcom-engine/pull/635) |
| FAF: `GetDepositsAroundPoint(x, z, radius, type)` in the sim and the UI, each deposit's whole-cell square as Moho keeps it, and the UI's `GetHighlightCommand()`, the order whose waypoint is under the cursor; and the cursor's `Position`, `Elevation` (the ground, below any water) and `OCell` in `__EngineStats` (Camera, Cursor), in the forms of the dump of Moho's tree in FAF's `performance.lua`, which FAF's `GetMouseTerrainElevation` reads: what FAF's context-based templates hotkey asks for (not run with FAF's scripts here) | [#635](https://github.com/bwiemz/supcom-engine/pull/635) |
| FAF: `CopyToClipboard(text)` copies to the desktop's clipboard (false offscreen), so Ctrl-clicking a chat line copies it | [#635](https://github.com/bwiemz/supcom-engine/pull/635) |
| A platoon's `FindClosestUnit` and `FindPrioritizedUnit` read their alliance as `SCR_GetEnum` does (any case, prefix optional): retail's AI asks for `'enemy'`, which matched nothing, so `AttackForceAI`'s final push was handed its own closest unit and its attack was refused | [#605](https://github.com/bwiemz/supcom-engine/pull/605) |
| `IssueFormAttack(units, target, formation, heading)` attacks in formation (`UNITCOMMAND_FormAttack`): the formation centred on the target, a unit or a point, each unit heads for its slot until its weapon reaches the target, then attacks as any other; one with no weapon that can hit the target gives the order up unless it is `DIRECTFIRE`. It returns nothing | [#605](https://github.com/bwiemz/supcom-engine/pull/605) |
| `_c_CreateDecal` (`UserDecal`): the area-weapon and nuke target reticle follows the cursor | [#502](https://github.com/bwiemz/supcom-engine/pull/502) |
| `GetBoneDirection(bone)` returns three numbers, not a vector table: retail's Lua destructures `v.x, v.y, v.z = GetBoneDirection(b)`, so the Scathis's `CreateProjectileAtMuzzle` errored in `GetAngleInBetween` and the weapon never fired | [#509](https://github.com/bwiemz/supcom-engine/pull/509) |
| A right click on an enemy is an attack only if a selected unit can hit it (`UserUnit:CanAttackTarget`); on one none can, the cursor is `RULEUCC_Invalid` and no order is given (tanks on a bomber) | [#531](https://github.com/bwiemz/supcom-engine/pull/531) |
| Projectile `RotationalVelocity`, `RotationalVelocityRange`: a projectile spins about a random axis from its creation (death debris, cluster bomblets, Kril torpedo) | [#532](https://github.com/bwiemz/supcom-engine/pull/532) |
| `brain:GetUnitsAroundPoint(cat, pos, r)` and `GetNumUnitsAroundPoint` with no alliance take any army's units the brain knows of (its own, or one it holds a blip of); with one, only the known units of that alliance (the Ythotha's energy being) | [#533](https://github.com/bwiemz/supcom-engine/pull/533) |
| `brain:GetListOfUnits(cat, needToBeIdle, requireBuilt)`: `needToBeIdle` leaves out units with orders, and `requireBuilt`, when not given, is true (retail's `OnDefeat` no longer kills unfinished units; FAF's wrapper passes false) | [#534](https://github.com/bwiemz/supcom-engine/pull/534) |
| Platoon orders take their squad argument (`MoveToLocation`, `MoveToTarget`, `AggressiveMoveToLocation`, `Patrol`, `AttackTarget`, `GuardTarget`, `Stop`); with none they go to squads Attack to Scout (`AttackTarget`, `GuardTarget`: Attack and Artillery), and with a formation override to the whole platoon (campaign base transports' routes go to the transports only) | [#535](https://github.com/bwiemz/supcom-engine/pull/535) |
| `SetElevation` and `RevertElevation` set and restore the height an aircraft flies at (FAF's Cybran build drones drop to 1 on their way home) | [#595](https://github.com/bwiemz/supcom-engine/pull/595) |
| `CombatTurnSpeed`, `TightTurnMultiplier`, `MinAirspeed`, `CirclingTurnMult`: attack runs and circling drive the same attitude controller as ordinary flight, toward the velocity ComputeAirCombatTactics or CalcCirclingOrientation asks; a fighter or bomber banks by its BankFactor in a run, ten times over in a NormalTurn, and chases a moving air target at its distance a second, at least MinAirspeed | [#562](https://github.com/bwiemz/supcom-engine/pull/562) |
| `KTurn`, `KTurnDamping`, `KRoll`, `KRollDamping`, `BankFactor`, `BankForward`: an aircraft turns and rolls under KTurn and KRoll against their dampings toward the attitude CalcWingedOrientation or CalcHoverOrientation asks, its turn error held to TurnSpeed; a winged one banks into its turn by BankFactor, a hovering one leans into its change of velocity (forward too only with BankForward); near its goal it faces the way its order sent it, stopped it holds on the arc its turn would carry it, and it flies at its height to a landing place more than StartTurnDistance away | [#546](https://github.com/bwiemz/supcom-engine/pull/546) |
| `KMove`, `KMoveDamping`: an aircraft's level velocity answers KMove toward the lesser of its distance and its top speed against CalcAirMovementDampingFactor, so it brakes onto the centre of its goal's cell, and with no move to follow holds where its velocity would carry it in a second; it flies through at speed only with another move, attack, patrol or guard order queued, on guard, or, with orders or none, when it finds nowhere to land or has `ForceSpeedThrough` (UpdateSpeedThroughStatus), so one idle over deep water flies on round its place, trying to land again each time it comes back, and one guarding circles what it guards; it lands under the same controller | [#544](https://github.com/bwiemz/supcom-engine/pull/544) |
| `KLift`, `KLiftDamping`, `LiftFactor`: an aircraft climbs under KLift over its transport load, against KLiftDamping, toward the highest ground ahead tracked at LiftFactor a second, and slows for ground rising more than LiftFactor | [#537](https://github.com/bwiemz/supcom-engine/pull/537) |
| A paused unit's army pays what its script leaves asked: a shield's or radar's upkeep goes on, a build stops costing through `OnPaused`, and an enhancement begun while paused costs without progress; sim `unit:SetPaused` calls `OnPaused`/`OnUnpaused`, and an enhancing unit is `Upgrading` | [#577](https://github.com/bwiemz/supcom-engine/pull/577) |
| A paused unit moves, reclaims and fights; a paused builder, repairer or factory keeps its order and state but does no work, and starts nothing new until a retry after it unpauses; a frame whose only builder is a paused engineer decays (FAF's exe also stops a paused unit's prop reclaim) | [#557](https://github.com/bwiemz/supcom-engine/pull/557) |
| An unfinished unit no builder works on decays from its second tick, by 0.1 / max(BuildCostEnergy, BuildCostMass, BuildTime) a tick, and calls `OnDecayed` at no health | [#538](https://github.com/bwiemz/supcom-engine/pull/538) |
| A builder arm calls `OnStartBuilderTracking` and `OnStopBuilderTracking` as its heading starts and stops turning, and turns back to rest at once at a quarter of its slew; `ConstructionUnit` folds the arm after a build | [#539](https://github.com/bwiemz/supcom-engine/pull/539) |
| Order validity: Reclaim, Capture and Repair go only to a unit of `RECLAIM`, `CAPTURE`, `REPAIR` (not a docked `POD`); builds only to a `FACTORY`, `ENGINEER`, `NEEDMOBILEBUILD` or `POD` (the UEF support commander's pod no longer captures) | [#542](https://github.com/bwiemz/supcom-engine/pull/542) |
| FAF: `CQUEMOV` makes a half-built unit selectable and able to take orders, as `FACTORY` does (a half-built mex takes its upgrade and starts it once finished) | [#543](https://github.com/bwiemz/supcom-engine/pull/543) |
| `NeedToFaceTargetToBuild` turns a builder to its site or repair target before it builds (retail's Seraphim engineers) | [#550](https://github.com/bwiemz/supcom-engine/pull/550) |
| `CreateAnimator(unit, true)`: a walk cycle plays at the unit's speed over its MaxSpeed, at least a quarter while it turns | [#551](https://github.com/bwiemz/supcom-engine/pull/551) |
| A surface unit running into a prop calls its `OnCollision` (tanks knock trees over) | [#590](https://github.com/bwiemz/supcom-engine/pull/590) |
| `MotorFallDown:Whack` tips a tree over tick by tick (a whack that doesn't break it sways it back) | [#553](https://github.com/bwiemz/supcom-engine/pull/553) |
| `SHOWQUEUE`: `SetCurrentFactoryForQueueDisplay` shows the queue only of a unit with the category, and gives `nil` for an empty one | [#555](https://github.com/bwiemz/supcom-engine/pull/555) |
| `RequestRefreshUI()` on a selected unit reports the selection to `OnSelectionChanged` again (enhancements, adjacency, transport loads) | [#556](https://github.com/bwiemz/supcom-engine/pull/556) |
| `Prop:AddBoundedProp(priority)` caps wrecks at 1000, destroying the cheapest, oldest one | [#560](https://github.com/bwiemz/supcom-engine/pull/560) |
| Projectile `StrategicIconName`: nukes and anti-nukes show their icon in the strategic view, pulsing in the bloom (`UI_RenProjectileIcons`, `UI_RenProjectileGlow`) | [#576](https://github.com/bwiemz/supcom-engine/pull/576) |
| Projectile `StrategicIconSize`: a projectile without an icon name is a square in the strategic view (yellow, as retail's `UI_forceWeaponsToYellow` defaults) | [#561](https://github.com/bwiemz/supcom-engine/pull/561) |
| `RaisedPlatforms`: a unit on a factory's or gateway's deck stands on its platform quads, and rolls off down its ramps | [#564](https://github.com/bwiemz/supcom-engine/pull/564) |
| `FAVORSWATER`: the Salem paths on its water footprint while it and its order's destinations are on water | [#565](https://github.com/bwiemz/supcom-engine/pull/565) |
| FAF: `Unit:ForceAltFootprint(bool)` holds a unit to its alt footprint (the Salem's amphibious toggle) | [#565](https://github.com/bwiemz/supcom-engine/pull/565) |
| `CreateThrustController` turns its engine bone toward the unit's thrust within `SetThrustingParam`'s caps, and an animator plays on top of the manipulators before it (retail UEA0104, UEA0107, XEA0306; FAF's UEA0203, UEA0305) | [#567](https://github.com/bwiemz/supcom-engine/pull/567) |
| `CameraFollowsProjectile`, `CameraFollowTimeout`: a camera following a unit goes on to such a shot of it, and back to the unit the timeout after the shot is gone (nukes, tactical missiles, artillery shells, bombs) | [#568](https://github.com/bwiemz/supcom-engine/pull/568) |
| `TeamColorMode`: the strategic icons (world view and minimap) in `GameColors.TeamColorMode`'s Self, Ally, Enemy and Neutral by the focus army's alliances; FAF's per-army colour string | [#570](https://github.com/bwiemz/supcom-engine/pull/570) |
| `RenderOverlayEconomy`: the focus army's units drawn as meshes show their net energy and mass per second (`/lua/ui/game/econoverlayparams.lua`) | [#571](https://github.com/bwiemz/supcom-engine/pull/571) |
| The engine's own HUD text (`--legacy-hud`'s economy bar and selection panel, the profiler overlay) stands on its font's baseline | [#575](https://github.com/bwiemz/supcom-engine/pull/575) |
| `UNTARGETABLE` on units: the cursor passes through one that is neither `SELECTABLE` nor `FERRYBEACON` (URA0001, URB5206, XSC9010/9011, the Othuy XSL0402) | [#536](https://github.com/bwiemz/supcom-engine/pull/536) |
| Projectile `Physics.LeadTarget` defaults to true (homing missiles and torpedoes lead a moving target) | [#529](https://github.com/bwiemz/supcom-engine/pull/529) |
| `PrefersPrimaryWeaponTarget` (a weapon takes its primary's target when it can hit it) and `StopOnPrimaryWeaponBusy` (it drops its target while the primary has one): ships, gunships, ASF, experimentals; Janus and Notha bombs | [#549](https://github.com/bwiemz/supcom-engine/pull/549) |
| Texture scrollers (`AddThreadScroller`, `AddPingPongScroller`, `AddManualScroller`, `RemoveScroller`) and the mesh LOD's `Scrolling`: tank treads (36 retail blueprints with `Treads.ScrollTreads`) and UEA0102's ping-pong scroll move | [#540](https://github.com/bwiemz/supcom-engine/pull/540) |
| `FlattenMapRect(x, z, sx, sz, y)` writes the heightfield: a FlattenSkirt structure levels the ground under its skirt, the land units on it follow, and a save keeps it | [#541](https://github.com/bwiemz/supcom-engine/pull/541) |
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
| `SELECTABLE`: only a unit with it can be selected (a transport beacon can't) | [#512](https://github.com/bwiemz/supcom-engine/pull/512) |
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
