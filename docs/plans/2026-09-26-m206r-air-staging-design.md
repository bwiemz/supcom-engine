# M206r: air staging (dock, refuel, repair)

Aircraft dock at air staging platforms, refuel and repair there, and leave when
done, as Moho's `CUnitRefuel` task and `CUnitMotion::ProcessFuelLevels` do. The
reference is faf-re's decompiled engine (`CUnitRefuel.cpp`,
`CUnitMotion.cpp`, `IAiCommandDispatchImpl.cpp`, `CAiTransportImpl.cpp`,
`CUnitUnloadUnits.cpp`, and `cfunc_IssueDockCommandL` in
`CCommandLuaFunctionRegistrations.cpp`).

## What the engine lacks today

- There is no Dock order, and `IssueDockCommand` is unbound, so the orders
  panel's Dock button does nothing.
- Retail's AI refuels aircraft with `IssueTransportLoad({unit}, platform)`,
  then releases them with `IssueTransportUnload({platform}, pos)`
  (`AIBehaviors.lua` `AirUnitRefit` / `AirStagingThread`). The load is
  handled as a transport call, which a platform (a structure) cannot answer.
  The unload makes the platform try to fly to the drop point.
- Fuel only drains. Nothing recharges it, and an aircraft's script, which
  slows it when dry (`OnRunOutOfFuel`), is never told it has fuel again.

## Moho's rules

**Routing.** `UNITCOMMAND_Dock` and `UNITCOMMAND_TransportLoadUnits` share one
dispatch arm. A target in `AIRSTAGINGPLATFORM` gets the refuel task
(`IssueRefuelTask`). A `CARRIER` target gets it too, for Dock; for a load it
gets the carrier landing task.

**The refuel task** (the pad branch). Task return codes follow Moho's scheduler:
1 is the next tick, and n > 1 is n − 1 ticks later.

1. *Preparing:* ask the pad for a slot (`TransportAssignSlot(unit, -1)`, the
   same class-bucketed bones a transport uses). On success, go on in the same
   tick. Otherwise, a patrolling or guarding unit gives up; any other unit
   heads for the pad and asks again 9 ticks later.
2. *Waiting:* fly to the slot's bone, down to its height, turning to face the
   way the bone faces (its local +Z, flattened).
3. *Starting:* once facing within `dot > 0.95`, attach to the bone.
4. *Processing:* every 9 ticks, when `FuelRatio > 0.99` and health is full,
   detach and lift back to the air layer.
5. *Complete:* clear the `Refueling` state and end the task.

The task ends when the aircraft or the pad dies, or when the pad is not idle
(it has an order). It also ends when the pad submerges, which releases a
docked aircraft first. While attached, a unit's command dispatch is blocked,
so no new order starts until it is released. The task already running keeps
ticking.

**Fuel** (`ProcessFuelLevels`, every tick when `FuelUseTime > 0`):

- Docked at a staging platform: fuel rises by
  `FuelRechargeRate / FuelUseTime × 0.1 × platform AI.RefuelingMultiplier`,
  up to 1.
  - `OnStartRefueling` fires once per docking if the tank was not full.
  - A damaged aircraft makes an economy request for the platform's
    `AI.RepairConsumeEnergy` / `RepairConsumeMass`. Each full grant heals
    `AI.RefuelingRepairAmount × 0.1`.
- Landed but not docked: the same rise, without the multiplier and scaled by
  a further 0.1. The engine has no landed state for aircraft, so this case
  does not arise.
- Otherwise: fuel falls by `1 / (FuelUseTime × 10)`.
- Callbacks: `OnRunOutOfFuel` fires when fuel falls to 0 from above.
  `OnGotFuel` fires when it rises from exactly 0.

**Unloading a pad** (`CUnitUnloadUnits` with `mIsStagingPlatform`). The pad
does not move. It detaches its aircraft, which keep their height, and orders
them to move to the unload point, clearing their queues.

**`IssueDockCommand(clear)`** (UI):

1. Take the dock-capable units in the selection (`RULEUCC_Dock`). Find the
   centre of their positions, or of their last queued orders when not
   clearing.
2. Scan the focus army's `AIRSTAGINGPLATFORM` units that are:
   - alive and not being built;
   - not on the Sub or Seabed layer;
   - idle.
3. Each platform's capacity is its `Transport.DockingSlots`. A carrier's is
   `StorageSlots` minus what it holds.
4. If the nearest platform holds them all, dock them all there. Otherwise
   share them out, roomiest first, among the platforms within the nearest
   one's distance plus 100. Each platform gets
   `round(capacity × max(1, units / total capacity))` units.

## The engine's version

- **`CommandType::Dock`**, the UI's order. `run_order` sends Dock and
  TransportLoad to `order_transport_load`, which hands a staging-platform
  target (not a carrier) to the new `order_refuel`.
- **`order_refuel`** is the pad branch of the task. It runs on the order's
  runtime state (`dock_phase`, `dock_wait`), with the transport pickup's
  pieces:
  - the slot comes from `TransportSlots::assign` on the pad;
  - the approach and climb use the navigator and `hold_altitude`;
  - the attach and release use `attach_to_transport` and `detach_cargo`.
- **Docked units keep ticking.** An attached unit skips its orders. Docked at
  a pad, it runs its refuel order (if that is still its head), its fuel and
  its repair. A docked unit whose order was replaced waits on the pad, as in
  Moho.
- **Slots.** A pad's slots are freed by the existing stale-slot sweep, which
  now counts a unit whose head order is a refuel onto that pad as still
  coming.
- **Aircraft leaving a pad keep their height.** `detach_cargo` sets cargo on
  the ground, which is right for a transport's cargo and wrong for an
  aircraft.
- **Repair** is metered like a silo's missile. The docked unit asks its army
  for the pad's `RepairConsume*` per tick (×10 per second), and heals
  `RefuelingRepairAmount × 0.1 × efficiency` per tick. This is the average of
  Moho's grant-then-heal cycle.
- **Fuel** follows `ProcessFuelLevels`, as above. `Physics.FuelRechargeRate`
  is read at spawn beside `FuelUseTime`.
- **Unloading a pad** follows `CUnitUnloadUnits`: the pad detaches its
  aircraft where they sit, and each is given a Move to the drop point.
- **`IssueDockCommand`** follows Moho's scan and share-out. The engine does
  not play the voice-overs.

## Left for later

- **Carrier landing** (`CUnitCarrierLand`): its storage reservation, its
  approach and its descent to the deck. Until it is done, a Dock order to a
  carrier ends at once, and the UI's scan leaves carriers out.
- **Patrol auto-refuel** (`Unit::FindPlatform`, `NeedRefuelThresholdRatio`).
- **Voice-overs** for no staging platforms and busy staging platforms.

## Verification

A data test (`--air-staging-test`) on SCMP_009:

1. Build a pad and damaged, half-fuelled aircraft. Dock them with the UI's
   `IssueDockCommand`, and one with the AI's `IssueTransportLoad`.
2. Check that each unit:
   - gets a slot;
   - attaches facing its bone;
   - refuels at the rate above (checked tick by tick);
   - repairs while its army pays;
   - leaves once full.
3. Check that a pad unloaded by the AI releases its aircraft to the drop
   point, and that a dead pad releases its reservations.

Mutants over the routing, the phases, the fuel rates, the repair and the scan
confirm that the test fails when each part is broken.
