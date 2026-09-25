# M206l — Transports carry by their attach points

Status: slice 1 (M206l, slots) done, 2026-09-25. Part of M206 (order fidelity).

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
