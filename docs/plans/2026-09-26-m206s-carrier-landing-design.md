# M206s: aircraft land on carriers

Aircraft land on a carrier and are stored inside it: when ordered to load
onto one, and when told to dock at one, where they refuel and repair before
they are launched again. This follows M206q (carrier storage and launch) and
M206r (air staging). The reference is faf-re's decompiled Moho:
- `CUnitCarrierLand` and `CUnitCarrierRetrieve`;
- `CUnitRefuel`'s carrier branch;
- `CAiTransportImpl::TransportReserveStorage`, `TransportHasAvailableStorage`
  and `TransportResetReservation`;
- the Dock/TransportLoadUnits dispatch in `IAiCommandDispatchImpl`.

## Moho's rules

**Reservation.** `TransportReserveStorage(unit)` takes the carrier's next
generic attach point, round robin. On retail carriers these are its
`Attachpoint` bones, since `ClassGenericUpTo` is 2 or 3.

1. The unit joins the reserved set.
2. The call returns the bone's world position, its forward (the facing), and
   its height over the carrier's origin.
3. It also returns a queue delay: 0 on the first pass over the points,
   rising by 3 with each full pass, modulo 50.

`TransportHasAvailableStorage` counts reservations as taken: `stored +
reserved < StorageSlots`. Storing a unit (`TransportAddToStorage`) ends its
reservation. `TransportResetReservation` resets the round robin, the
overflow and the launch index.

**Landing** (`CUnitCarrierLand`, the aircraft's task). It sets
`TransportLoading` on the unit and focuses the unit on the carrier.

1. *Preparing:*
   - With no storage free, the brain hears `OnTransportFull` and the task
     ends.
   - Otherwise the unit reserves a point. It computes an approach point 20 +
     the delay back from the point, along the point's flattened facing.
2. *Waiting:* it flies to the approach point at the point's height,
   facing the way the point does.
3. *Starting:*
   - While the carrier is under water, it checks again 4 ticks later
     (`return 5`).
   - Otherwise it fixes the deck height (the carrier's y + the height) and
     waits the delay (`return delay`, or 1 when the delay is 0).
4. *Processing:* it comes down onto the point.
5. *Complete:* the carrier stores it, and the task ends.

The task ends early when the carrier dies. A refuelling unit also gives up
when the carrier is busy or diving. An ordered unit gives up, once past
Preparing, when the carrier stops loading (its `TransportLoading` state).

**Retrieve** (`CUnitCarrierRetrieve`, the carrier's side of an ordered
load, `IssueTransportLoad({planes}, carrier)`). The carrier runs its own
share of the load order.

1. Setup and teardown:
   - The carrier hears `OnStartTransportLoading` and is `TransportLoading`
     while the task runs.
   - When the task ends, it hears `OnStopTransportLoading` and resets its
     reservations.
   - If the task ends unfinished, the tracked units stop moving.
2. *Preparing:* wait until every tracked unit's current order is the
   carrier's. A submerged carrier then surfaces; any other carrier stops
   moving.
3. *Starting:* every 9 ticks, drop tracked units that are dead or no longer
   loading. Units still loading and focused on the carrier are kept. With
   none left, the task is done.

**Docking at a carrier** (`CUnitRefuel` with `mIsCarrier`):

1. *Preparing:*
   - With storage free, the unit lands (the task above, as a refuel).
   - Otherwise it heads for the carrier and asks again every 9 ticks.
2. *Waiting:* while it is stored, every 9 ticks it checks for a tank over
   0.99 and full health. Once both hold, the carrier launches it from its
   next launch bone at top speed (`TransportRemoveFromStorage`).
3. *Starting:* the Refueling state goes, and the task ends. While another
   aircraft on the same order is still refuelling, the unit circles the
   carrier first.

Stored units refuel and repair as docked ones do: `GetStagingPlatform` is the
carrier (M206r).

## The engine's version

- **The carrier.** `Unit` gains the reserved set, the round robin and the
  overflow: `reserve_storage`, `clear_reservation` and `reset_reservation`.
  - `transport_has_available_storage` counts the reserved set.
  - `add_to_storage` clears the unit's reservation.
  - The stale-slot sweep drops the reservations of units no longer landing.
- **Routing** (`order_transport_load`):
  - A TransportLoad onto a CARRIER runs `order_carrier_land`, not the
    transport call.
  - The carrier's own TransportLoad, its share of the order, runs
    `order_carrier_retrieve`. `order_call_transport` gives the carrier its
    share, as it does a transport's.
  - A Dock onto a carrier runs `order_refuel`'s carrier branch.
- **The landing** is phase state on the order, as in M206r:
  - it reserves;
  - it flies to the approach point, coming down to the deck height;
  - it waits its delay;
  - it glides onto the point, then is stored.
  The engine flies and glides as M206r's approach does. Moho's carrier
  motion events (hold height relative to the carrier) aren't modelled: the
  carrier holds still while aircraft land, since the retrieve stops it and a
  refuel needs it idle.
- **Docking** reuses `order_refuel` with a carrier flag: it lands, waits
  stored until full, then leaves through `remove_from_storage`.
- **The UI's Dock** takes carriers now, with room `StorageSlots` minus what
  each stores.

## Left for later

- A carrier holding its aircraft's height as it moves. Moho's carrier motion
  events aren't modelled, and the carrier holds still.
- Patrol auto-refuel (`FindPlatform`).
- The voice-overs.

## Verification

A data test (`--carrier-land-test`) on SCMP_009's water with a Cybran
carrier:

1. Reservation. The round robin covers the points, the queue delay rises
   after a full pass, and reserved storage counts as taken.
2. An ordered landing (`IssueTransportLoad` onto the carrier). The carrier
   stops and hears the loading callbacks. The planes approach, wait their
   delays and are stored, and the carrier's order ends.
3. A Dock at the carrier. The plane lands, refuels and repairs stored, and
   is launched when full.
4. A carrier that goes mid-landing ends the task.

Plus a game-UI case: Dock picks a carrier when it is the nearest platform.
Then mutants over each rule.
