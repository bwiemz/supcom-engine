# M206v: units change army by replacement

`ChangeUnitArmy` is how every unit changes hands in FA:
- captures (`Unit.OnCaptured`);
- gifts and defeat sharing (`SimUtils.TransferUnitsOwnership`);
- scenario scripts (`ScenarioFramework.GiveUnitToArmy`).

In Moho it makes a **new** unit for the new army and destroys the old one.
The engine changed the army of the same unit in place. So after a transfer:
- the unit kept its old owner's Lua state: script threads, AI manager data
  (`BuilderManagerData`), callbacks and trash;
- the script's re-application of veterancy, enhancements, fuel, silo ammo
  and shield (retail and FAF `TransferUnitsOwnership`) landed on a unit that
  already had them.

A FAF game found it. An AI captured an engineer, and the engineer's old
manager thread went on tasking it for a base that had been torn down, which
logged 1,700 "Invalid location" warnings.

The reference is faf-re's decompiled Moho:
- `Sim::TransferUnit` (`Sim.cpp`);
- `cfunc_ChangeUnitArmyL`.

## Moho's rules

**`ChangeUnitArmy(unit, army)`.**
- It takes two arguments, and the army index must be valid.
- A unit that already belongs to that army is a Lua error ("Unit already
  belongs to army N").
- If any unit attached to it is a COMMAND unit, it returns nil and nothing
  changes.
- Otherwise it returns `TransferUnit`'s result: the new unit, or nil.
- The retail binary computes an "ineligible source" test (being built, dead,
  COMMAND), but its branch was patched to no-ops, so it never gates anything.

**`Sim::TransferUnit(unit, army)`.**
1. It returns nil for a dead unit, or one whose destruction is queued.
2. **Stored units** (a carrier's storage): each is removed from storage,
   then transferred (recursively).
3. **Attached units** (cargo; mobile, alive, not being destroyed): their
   attach bones are recorded, then each is detached and transferred
   (recursively).
4. **The replacement** is created for the new army with the old unit's
   blueprint, transform and layer. Its elevation is fixed: it keeps the old
   height. It is created complete.
   - If creation fails (the unit cap), the army's brain hears
     `OnFailedUnitTransfer`, unless the army ignores the cap, and the
     result is nil.
5. **Carried over:**
   - the animation poses;
   - health (set when it differs);
   - the custom name.
6. **A transport's cargo returns:**
   - the transferred stored units go back into the new unit's storage;
   - each transferred attached unit is attached at its old bones, given the
     slot at that bone, and the new transport's script hears
     `OnTransportAttach(bone name, unit)`.
7. **The old unit is destroyed** (a plain `Destroy`, not a kill). A
   structure's old footprint is not lifted, since the new unit stands on it.

Everything else is left to the scripts, which re-apply it to the returned
unit: veterancy, enhancements, fuel, silo ammo, shield health and state,
factory progress, and FAF's `OnGiven`.

**There is no fallback.** Moho's capture task calls the target's
`OnCaptured(captor)` and nothing else; the script's `ChangeUnitArmy` does
the transfer.

## The engine

- **`transfer_unit`** (`sim_bindings.cpp`, beside the creation code)
  follows `TransferUnit`:
  - stored units come out through `remove_from_storage`, go back in
    through `add_to_storage`;
  - cargo comes off through `detach_cargo` (left where it hung) and goes
    back on through `attach_to_transport`, in the order its slots were
    given, so each unit gets the same slot on a fresh transport;
  - the replacement is made by `spawn_complete_unit`, the core of
    `CreateUnit` (OnPreCreate, weapons, OnCreate, OnStopBeingBuilt,
    footprint, adjacency, ArmyPool). Before its scripts run it takes the
    old unit's position, orientation and layer, and, for aircraft, altitude
    and heading.
- **`ChangeUnitArmy`** follows Moho's checks and returns the new unit.
- **Capture:** the C++ fallback goes.
- **The old unit's `Destroy`** is marked as a hand-over
  (`Unit::set_transferred`). It skips what a lost unit gets: the
  `Units_Killed` count and the renderer's death flash.
- **Its footprint** needs nothing special. The pathfinding grid counts
  claims per cell, so the old unit's release leaves the cells blocked by
  the replacement's own claim. Moho's grid doesn't count claims, which is
  why its transfer tells the old unit not to lift its footprint. Doing
  that here (dropping the old claim) would leave the cells blocked for good
  once the replacement dies; the first version did, and a mutation test
  caught it.
- **The engine's share on defeat** (`SimState::dispose_defeated_army`, for
  its victory check and a dropped player) gives units through the
  `ChangeUnitArmy` global, after its walk over the units, since a transfer
  makes units. A sim without the bindings (unit tests) still moves them
  in place.
- **`AddOnGivenCallback` goes.** Moho has no such unit method, and retail
  has no `OnGiven`. FAF defines its own in `Unit.lua` and calls `OnGiven`
  from `TransferUnitsOwnership`. The engine's version fired from
  `ChangeUnitArmy`, which nothing but its own test relied on.

Gaps:
- **Poses.** The new unit's animators start over; its scripts set its pose
  up again, as they do for any new unit.
- **The unit cap.** The engine doesn't enforce it yet (`ArmyBrain::unit_cap`
  is informational), so `OnFailedUnitTransfer` never fires.
- **Motion.** A transferred unit starts at rest, as it does in Moho, which
  passes only the transform.

**Import order** (found by the same FAF game). `boot_sim` imported
`Unit.lua` before `SetupSession`, and pre-made an empty `ArmyBrains` so that
FAF's `SimUtils` (which `Unit.lua` imports) could read it. FAF's `SimUtils`
keeps `local ArmyBrains = ArmyBrains` at file scope. `SetupSession` then
makes a new `ArmyBrains`, so FAF's `TransferUnitsOwnership` saw no brains and
returned without calling `ChangeUnitArmy`. The engine's capture fallback
then changed the army in place. The script classes are now imported after
`SetupSession`, as Moho's first use of them is.

## Tests

**`--change-army-test`** (new, gate):
1. A hurt tank given to ARMY_2 comes back as a new unit, where it stood,
   with its health.
2. The old unit is destroyed, with its script field and its forked thread,
   and ARMY_1 doesn't count it lost.
3. Its custom name is carried over.
4. Giving it to its own army is an error.
5. A structure's replacement still blocks its footprint, and the ground
   clears when the replacement is destroyed.
6. A transport's two engineers are new units of the new army, aboard the
   new transport.
7. A transport carrying a commander stays as it is (nil).
8. An aircraft in flight keeps its height and heading.
9. A carrier's stored fighter is stored, as a new unit, in the new carrier.
10. A defeated army's units defect as new units.

**`--capture-test`** (rewritten): it used to log its failures and relied on
the in-place change. Now it asserts two things:
- a unit given to ARMY_2 is a new unit;
- captured back, retail's `OnCaptured` gives ARMY_1 a new power generator
  where it stood, with the health it was left at, not counted as ARMY_2's
  loss.

**`--medstub-test`** Test 4 is now "`ChangeUnitArmy` to its own army is an
error".

**FAF regression** (SCMP_009, seed 4242, 12,700 ticks), with only the
import-order fix:
- 28 `ChangeUnitArmy` calls from FAF's own scripts;
- no capture fallbacks;
- no "Invalid location" warnings (1,732 before).
