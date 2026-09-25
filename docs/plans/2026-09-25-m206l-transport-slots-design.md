# M206l — Transports carry by their attach points

Status: done, 2026-09-25: M206l (slots), M206m (the pickup) and M206n (the drop). Part of M206 (order fidelity).

Source: Moho's decompiled `CAiTransportImpl`, and the unit tasks `CUnitLoadUnits`, `CUnitCallTransport` and `CUnitUnloadUnits` ([faf-re](https://github.com/Draiget/faf-re)). faf-re is still reconstructing parts of the load handshake; it carries temporary probes there. So each rule below is checked against retail's data and a test, not taken on trust.

## What the engine does now

- **Capacity** is the blueprint's `Transport.Class1Capacity`, and a missing one means no limit. No retail transport has the field, so on retail every transport carries any number of units, of any size.
- **Boarding:** a unit boards by jumping to the transport's origin. Scripts hear `OnTransportAttach("Attachpoint", unit)`, whichever bone it is.
- **Carrying:** carried units sit at the transport's origin, facing the way they last faced.
- **Unloading:** they are set down at the transport's origin, on top of each other.

## What Moho does

### Attach points (`CAiTransportImpl::SetUpAttachPoints`)

The transport's bones are sorted by name when it is made. The tests are case-sensitive `strstr`, in this order; the first match wins:

| Bone name contains | List | Pooled into the generic list when |
|---|---|---|
| `Launchpoint` | launch points | never |
| `Attachpoint_Spr` | class 4 | `Transport.ClassGenericUpTo` ≥ 4 |
| `Attachpoint_Lrg` | class 3 | `ClassGenericUpTo` ≥ 3 |
| `Attachpoint_Med` | class 2 | `ClassGenericUpTo` ≥ 2 |
| `Attachpoint` | class 1 | `ClassGenericUpTo` ≥ 1 |
| `AttachSpecial` | special | never |

Retail's transports bear this out:

| Transport | Small (class 1) | `_Med` | `_Lrg` |
|---|---|---|---|
| UEF T1 `uea0107` | 6 (`Left/Right_Attachpoint_sml_0N`) | 2 | 1 |
| UEF T2 `uea0104` | 14 (`Left/Right_Attachpoint0N`) | 6 | 3 |
| Cybran T1 `ura0107` | 6 (`Attachpoint_sml_`) | 2 | 1 |
| Aeon T1 `uaa0107` | 6 (`Attachpoint_Small_`) | 3 | 1 |
| Seraphim T1 `xsa0107` | 8 (`Left_Attachpoint`) | 4 | 1 |

### Slots (`TransportFindAttachList`, `GetClosestAttachPointsTo`, `TransportAssignSlot`)

- A unit's class is its blueprint's `Transport.TransportClass` (default 1).
- **Class `c` ≤ `ClassGenericUpTo`:** it takes one generic point.
- **Otherwise** it takes a point from class `c`'s list. When class `c` has an attach size (`Class2AttachSize`, default 2; `Class3AttachSize`, default 6, retail's transports 4; `Class4AttachSize`, default 1; `ClassSAttachSize`, default 0), it also takes that many "hook" bones: the class-1 bones nearest the point, or the generic ones when class 1 has none. Class 1 takes just its point.
- **A slot is free** when none of its bones is reserved. Assigning a slot reserves its bones for the unit. So a UEF T2 transport takes 14 small units, or 6 medium ones (2 hooks each of 14), or 3 large ones (4 each).
- **Checks:**
  - `TransportHasSpaceFor(unit)`: some slot for the unit's class is free.
  - `TransportCanCarryUnit`: the unit is mobile. It is land, or air for a staging platform. A commander rides only a `CANTRANSPORTCOMMANDER` transport. And its class has points, or enough hooks for its attach size.
- **The carried unit hangs by its own `AttachPoint` bone.** Failing that, a flier hangs by its root and anything else by its centre.
- **Scripts hear the bone's name:** `OnTransportAttach(boneName, unit)` and `OnTransportDetach(boneName, unit)`.

### Loading and unloading (later slices)

- **Loading** (`CUnitLoadUnits`, `CUnitCallTransport`):
  1. The transport assigns slots to its requested units, largest first (volume × density; nearer first on ties). A unit that gets none is dropped, and the transport hears `OnTransportFull`.
  2. The transport flies to the pickup centre, the mean of the assigned units' positions.
  3. Each unit walks to the ground under its bone.
  4. Within twice the transport's footprint of it, the unit beams up over 10 ticks and attaches.
- **Unloading** (`CUnitUnloadUnits`): the transport comes down. Each unit is detached where it hangs, if its footprint fits there, and drops to the ground.

## The slices

1. **M206l, this one: slots.**
   - `sim::TransportSlots` (unit-tested) sorts the bones and assigns, reserves and releases slots.
   - A transport with attach bones loads only into a free slot: a load order, a ferry's boarding, and `AddUnitToStorage`.
   - `TransportHasSpaceFor` asks the slots.
   - A carried unit hangs from its bone, facing as the bone does.
   - Unloading sets each unit down level where it hung, releasing the slot.
   - Scripts hear the real bone.
   - A transport without attach bones (no mesh, or a test's synthetic unit) keeps `Class1Capacity`.
   - The slots (who holds which bone) go into the checksum.
2. **M206m, the pickup:** the load handshake, with the transport at the pickup centre, units walking to their points and beaming up. The handshake also gates who may board, using `TransportCanCarryUnit`'s whole test (`TransportSlots::can_carry_class` is its class half). Today only a ferry checks part of it: it takes land units only, and a commander only onto a `CANTRANSPORTCOMMANDER` transport.
3. **M206n, the drop:** unloading from the ground.

## M206m — the pickup

Moho's `IssueTransportLoad(units, transport)` gives one `UNITCOMMAND_TransportLoadUnits` command to the units and to the transport together (`cfunc_IssueTransportLoadL`). The transport runs `CUnitLoadUnits` for it, and each unit runs `CUnitCallTransport`. Each side goes on only while the other's head command is the same order.

- **The transport gets the order from the units calling it.** A unit whose transport has no load order on itself queued appends one. That covers every path that gives units a load order: scripts, players, and a factory's rally orders, which the engine hands to built units directly.
  - A unit whose load order targets itself runs the transport's side.
  - The handshake is the heads: the transport's head is a load order on itself, and a unit's head is a load order on that transport. Ids aren't compared, since the engine numbers an order per unit on the script path.
- **The transport's side (`CUnitLoadUnits`):**
  1. **Start.** It takes the `TransportLoading` state and hears `OnStartTransportLoading`.
  2. **Hold.** Its requested units are the army's units with a load order onto it anywhere in their queues. It holds, as Moho's does, until each has that order at its head.
  3. **Assign.** They are sorted by size (SizeX × SizeY × SizeZ × `AverageDensity`; defaults 1 and 0.49), largest first. Ties go to the nearer, then the older, so every platform agrees. Each is given a slot, reserved now. A unit that gets none is dropped, and if any was, the transport hears `OnTransportFull`. With none assigned, the order ends, aborted.
  4. **Fly.** It hears `OnTransportOrdered` and flies to the pickup centre, the mean of the assigned units' positions. A transport that can't fly waits where it is.
  5. **Land.** Over the centre, it comes down to `Air.TransportHoverHeight` (default 0: it lands). Only then is it at the pickup: Moho's move there is onto the land layer.
  6. **Wait.** It hovers while its units board, up to 300 ticks.
  7. **End.** It releases the slots of any unit that never came. It hears `OnStopTransportLoading`, and `OnTransportAborted` if it timed out or was stopped. It then leaves the `TransportLoading` state. Idle with cargo, it stays at its hover height until moved (`ShouldHoverInsteadOfLand`).
- **A unit's side (`CUnitCallTransport`):**
  1. Until its transport starts the pickup, it waits.
  2. Once it has a slot, the order goes on only while the transport is still loading. A unit the transport gave no slot is left, its order done.
  3. Until the transport is at the pickup, it walks to the centre plus twice its bone's offset (`TransportGetPickupUnitPos`). After that it walks to the ground under its bone (`TransportGetAttachPosition`).
  4. Within twice the transport's footprint of the bone (horizontally), it beams up. It hears `OnStartTransportBeamUp(transport, boneIndex)`, and eases over 10 ticks, by `cos(t·π/10)/2 + 1/2`, from where it stood to the bone's place less its own height.
  5. It hears `OnStopTransportBeamUp` and attaches.
- **Stale state:**
  - A transport whose head is no longer that load order gives up its pickup, aborted.
  - A unit whose order ends mid-beam comes back down to the ground, and hears `OnStopTransportBeamUp`.
  - The stale-slot pass keeps a slot held for a unit still coming.
- **Proof:** `data.transport-pickup-test` (retail).
  - **The pickup:** a UEF T1 transport given 8 tanks takes 6 (`OnTransportFull`), flies 45 to their centre, and hovers at exactly 3. Each tank beams up, seen off the ground before it attaches, and hears a bone number. The 2 left end their orders.
  - **The abort:** a pickup stopped on the way is aborted, and its units don't call the transport back.
  - **Largest first:** a T3 bot boards ahead of tanks.
  - **Scripts that clear their orders:** a unit clearing its orders as it stops beaming up, and a transport doing so as it stops loading, still end the order safely. The unit boards, and no second order is taken off. Without the check, the process aborts.
  - **Mutations:** reversing the size order, keeping stopped units' orders, and loading before coming down each fail a test.

## M206n — the drop

Moho's `CUnitUnloadUnits` moves an aircraft to its drop onto the land layer, so a transport with cargo comes down to its `TransportHoverHeight`. Each unit is detached with `TransportDetachUnit`, which for an aircraft's cargo asks whether its footprint `FitsAt` where it hangs. A unit that doesn't fit stays aboard. Then the transport's target is the air again.

- **The order flies.** The unload order used the ground navigator, which dragged aircraft along the ground at a crawl. It now flies as the ferry and the pickup do.
- **`unload_step`**, shared by the unload order and the ferry's drop:
  1. The transport comes down to its hover height over the drop.
  2. It sets down the cargo (the order's units, or all of it) whose footprint fits the ground under it: every cell passable for a land unit (`Unit::footprint_fits`).
  3. The rest stays aboard, and the order ends.
- **Set down on the ground.** `detach_cargo` puts each unit on the surface where it hung, level. (Before, only the crowd-separation pass did that, and only for units near others.)
- **Idle.** A transport with cargo aboard hovers at its hover height, and an empty one climbs back to its flying height (`ShouldHoverInsteadOfLand`). This applies only to units with a hover height, so other aircraft are untouched.
- **A ferry whose cargo never fits at its drop** (over deep water, say) keeps flying its route with the cargo aboard. Moho's `CUnitFerryTask::HasNextUnitToLoad` sends a ferry that still has loaded units out along the route again, so this matches.
- **Proof:** `data.transport-drop-test` (retail).
  - A UEF T1 transport with 6 tanks flies up to 10, comes down to 3, and only then sets them down, on the ground and spread under it. Empty, it climbs back to 10.
  - Over deep water all 6 stay aboard, and it hovers at 3.
  - A lone tank, with none to jostle it, is set down on the ground.
  - **Mutations:** no descent, no fit check, no placement on the ground, and no idle climb each fail a check.

## Decisions

- **Ties in the hook sort:** Moho sorts by distance with MSVC's `std::sort`, which orders equal distances as its introsort happens to. The engine orders by distance and then bone index, a total order, so a slot is the same on every platform: a lockstep game must not depend on the standard library.
- **Carried units move with the transport in the same tick.** A carried unit hangs from its bone in its own tick, and the transport hangs its cargo again after it has moved. So a unit that ticks before its transport doesn't trail it by a tick, as Moho's attached entities don't.
- **Reservations:** only a unit aboard holds a slot in this slice. It is reserved when the unit attaches.
  - It is released when the unit is unloaded. A transport also gives up, each tick, any slot whose unit is no longer aboard: destroyed, dead, or taken off the cargo list. That is Moho's `TransportUnreserveUnattachedSpots`.
  - A ferry choosing boarders works on a copy of its slots. Those already boarding take theirs first, and a new unit boards only if one is left, so nothing is held for a unit that never arrives.

## Proof

- **Unit tests** over synthetic skeletons:
  - classification by name, including the first match winning and case sensitivity;
  - generic pooling;
  - multi-bone slots taking the nearest hooks;
  - reservation and release;
  - `HasSpaceFor` for each class;
  - the UEF T2 counts (14 / 6 / 3, and a mixed load).
- **Data-backed (retail):** a UEF T2 transport takes 14 T1 tanks and refuses the 15th. It takes 3 T3 units, and T1 units beside them only while hooks are left. Each carried tank's `AttachPoint` bone sits on its slot's bone, at rest and in flight. Unloaded units are spread out, not stacked. A unit destroyed aboard frees its slot.
- **The four-AI game:** no new Lua errors. The retail AI used no transports in 10 minutes, before or after.
