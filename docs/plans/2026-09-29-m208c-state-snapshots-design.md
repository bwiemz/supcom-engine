# M208c: State Snapshots — Design

**Status:** 2026-09-29. M208c-a (Lua persistence) implemented; M208c-b and M208c-c to come.
Roadmap Phase E (M208, save/load).

## Why

A save is the game's history: its setup and every command (M208a). Loading replays that
history to the saved tick. That approach is correct by construction, but a load costs what the
game took to play (M208b):

- A 30-minute four-AI game loads in minutes.
- A 60-minute one loads in about 40 minutes (Release, before M224).

Retail loads either at once, because Moho saves the sim's state. This milestone does the same:
a save carries a snapshot of the sim at the saved tick, and a load restores it. The history
stays in the save as the fallback and as the test oracle.

## The hard part: the future must not change

A restored game has to play every later tick exactly as the saved game would have. Otherwise
it desyncs from its own history, and from other peers in a multiplayer game. Four things make
this harder than writing fields out:

1. **Lua's table iteration order.** In Lua 5.0, `next`/`pairs` walk a table's node array. For
   string, number and boolean keys, where a key sits depends on the table's history: its
   insertions, deletions, resizes and `firstfree`. Re-inserting the keys doesn't reproduce
   that order.
2. **Object-keyed tables.** Keys that are tables, functions, threads or light userdata hash by
   address, which a new heap can't reproduce. Their tables' order already differs between two
   runs of the same build (ASLR moves the heap), and between OSes. Same-build replays and the
   cross-OS replay tests (M199) stay deterministic, so the sim doesn't depend on that order.
   The design relies on this and doesn't try to preserve it.
3. **Garbage-collection timing is part of the game.** Weak tables (trash bags, for example)
   lose entries at collections, so collections must fall on the same ticks. The sim forces a
   full collection every 70 ticks. Lua 5.0 also collects when its byte count (`nblocks`)
   reaches a threshold, and that count isn't game state:
   - it differs between builds (`Instruction` is 8 bytes on Linux and 4 on Windows);
   - it differs between runs (where an address-hashed key lands decides when its table
     grows);
   - a restored heap's differs again (it holds no garbage).

   The late game's heap never doubled between forced collections, so the threshold never
   fired. M208c-a makes that a rule: from tick 1 the sim collects only when forced
   (`lua_setmanualgc`). A restored heap then needs the same reachable objects, not the same
   byte count.
4. **Pointers between the two worlds.**
   - Lua tables hold C++ addresses: `_c_object` light userdata, on about a dozen kinds of
     object.
   - C++ holds Lua registry refs: every entity, weapon, manipulator, thread, platoon, brain,
     effect and economy event. Lua holds some of those ref numbers too (thread handles).
   - C functions sit in the heap.

## Shape of the solution

A snapshot has two parts, written together at a tick boundary:

- **The Lua heap:** everything reachable from the sim's registry and globals, and the
  per-type metatables.
- **The C++ sim:** every authoritative field of `SimState` and what it owns.

It is saved beside the history in the save file.

A load goes through these steps:

1. Boot the game's engine as for a new game: the same scenario, mods and blueprints, from the
   save's setup. This builds the C functions, terrain and blueprint store.
2. Replace the sim's C++ state with the snapshot's.
3. Replace the sim's Lua heap with the snapshot's, translating pointers.
4. Check the result: the restored state's checksum must equal the history's recorded checksum
   for the saved tick. On any mismatch or error, fall back to catching up from the history,
   as today.

### 1. Lua persistence (`third_party/lua-5.0/lpersist.cpp`)

The persister is written against our Lua 5.0, which carries the LuaPlus opcodes, per-type
metatables, the C++ error unwinding and M224g's collector (frozen tables, the lazy sweep). It
follows Pluto's model: objects are written once and then referenced by index, and threads are
written with their call frames. It differs from Pluto in three ways:

- it reproduces table layouts;
- it writes C functions by their place in the binary, not by name;
- it translates light userdata through the caller's hooks.

What is written is everything the collector's mark starts from, and everything those reach,
weakly held objects included:

- **Roots:** the registry, the main thread's globals, the per-type metatables and the frozen
  roots (`lgc.c`'s `markroot`).
- **Weakly held objects** stay until the next collection, in the saved game and the restored
  one alike.
- **Garbage** isn't written.
- **The main thread** must be at rest: nothing on its stack, no call under way.

- **Values:** nil, booleans, numbers (their bits), and strings (their bytes, re-interned on
  load).
- **Tables:**
  - The array part is written with its exact size.
  - The node part is written slot by slot: size (`lsizenode`), each slot's key, value and
    `next` index, and `firstfree`.
  - A table whose keys all hash by value is restored into the same slots, so it iterates
    identically.
  - A table with any key that hashes by address is rebuilt by inserting its entries in the
    saved iteration order, with its sizes kept (see point 2 above). Its value-keyed entries
    can't keep their slots either. An address key's new main position can land on a slot
    whose occupant isn't at its own main position. Lua's insert then moves that occupant and
    clears the slot's link, so a key chained from it would be lost (`ltable.c`'s `newkey`).
  - Metatable and weak mode are kept. Weak tables are written with their current entries,
    including ones whose objects are only weakly held. Those die at the next collection in
    both games.
- **Lua closures and prototypes:**
  - Prototypes are written whole: code, constants, nested prototypes, upvalue names, line
    info and locals, each array with its exact size. They are shared by identity, since
    modules imported mid-game exist only in the saved heap.
  - Closures hold their prototype and upvalues.
  - Upvalues keep their sharing. An open upvalue is written as its thread and stack slot, and
    is reopened on load.
  - An upvalue open on a thread the save doesn't reach is written closed, with its slot's
    value. That thread is garbage and never runs again, so nothing can tell the difference.
- **C functions and C closures:** as their offset from a function in the same binary. A
  snapshot is tied to its build anyway (see Risks), so no permanents table is needed, and the
  engine's anonymous lambdas need no names. A C closure also writes its upvalues.
- **Light userdata:** written through hooks the sim provides. Each `_c_object` address becomes
  a (kind, id) pair and back again (see below). NULL stays NULL. An address the hooks can't
  name fails the save. Host singletons (`osc_sim_state`, `osc_thread_mgr`, …) are re-pointed
  on load.
- **Full userdata:** their bytes as they are. The sim has none.
- **Threads (the sim's coroutines):**
  - The thread's status and stack are written with its size.
  - The CallInfo chain is written with base, top, `savedpc` as an offset, and state flags. The
    yielding C function on top is written by name.
  - Also written: the thread's open upvalues and its globals (`_gt`).
  - Suspended and waiting threads are restored exactly, so `lua_resume` continues where the
    saved game would have.
- **The registry** is persisted as a table like any other. Because its layout is kept, every
  integer ref C++ and Lua hold resolves the same after a load, and so does lauxlib's free list
  (`registry[1]`). The next `luaL_ref` hands out the same number the saved game would have.
- **The collector:**
  - Its modes are written: the lazy sweep, and collecting only when told.
  - A save doesn't drain a sweep under way. Everything it reads is reachable, so intact. The
    sweep frees only garbage, and a closure keeps the slot its open upvalue names marked.
  - Frozen tables come back frozen: fixed, on `frozengc`, with their strings fixed and their
    frozen roots in order. The load's own frozen tables thaw and are freed with the rest of
    the old heap.
  - A load collects the old heap at once. The new heap is marked beforehand, so the
    collection neither walks it nor clears its weak tables.
  - Byte counts aren't reproduced (see point 3 above). A later milestone may write frozen
    blueprint tables by blueprint id rather than by content, since the load's boot rebuilds
    them.

### 2. The C++ sim

Each system gets a serializer in its own file, written with the `ByteWriter`/`ByteReader`
the replays use. Each field is written in declaration order, and each class carries a version.

- **What's saved:** the fields the survey marks authoritative, about 600 across about 40
  classes. These include:
  - `SimState`'s rules, clocks, allocators and pause state;
  - the scheduler's queue with its sequence numbers;
  - the RNG;
  - every entity kind with its subsystems (navigator, weapons, manipulators with their
    cross-fade state, command queues with their runtime fields, transport, silo, carrier and
    ferry state);
  - brains, including platoons that were destroyed but kept for pointer stability;
  - influence maps, economy events, effects, the threads' C++ records, intel memory (blip
    cache, `los_ever`, `prev_entity_vis`), temporary visions, and the visibility grid's
    flags.
- **What's rebuilt:**
  - the pathfinding grid's obstacle marks, from occupied footprints;
  - spatial grids, id-order lists, category bits (their intern ids are per process), and bone
    and animation data pointers (from the caches);
  - pointer back-links such as `owner_` and `sim_`.
- **Order:** sequences are written in their order. Unordered maps are only ever looked up,
  never walked, so they are written sorted.
- **Ids:** every allocator (entity, effect, platoon, thread serial, command id, scheduler
  sequence, task serial, snap serials) is saved, so the future hands out the same ids.

**Pointer translation.** Lua's `_c_object` values name C++ objects by address. The persister
asks the sim for each address's (kind, id): entity, weapon (unit, index), navigator (unit),
manipulator (unit, index), platoon (army, index), brain (index), or economy event (index).
The load asks the other way. `_c_sim_gen` fields are rewritten to the new generation. The
light userdata in the per-type metatables and caches are re-pointed.

### 3. The save file and the load

- **The save file.** `SavedGame` gains an optional snapshot section, compressed, beside the
  history. A save without one, such as an older save, still loads by catching up.
- **Saving.** Snapshots are taken at a tick boundary, never inside one. The sim writes one
  when the UI's save request is processed; `InternalSaveGame` doesn't change.
- **Loading.**
  - Restore, then compare the restored state's checksum parts with the history's for the
    saved tick.
  - On a match, the player takes over at once.
  - On a mismatch, an error, or a snapshot from another build, fall back to catching up and
    log why.
- **Multiplayer.** Every peer restores the same snapshot, and a lockstep game compares
  checksums from the next tick. Retail doesn't load multiplayer games; this is here for
  completeness.

## Proof

**The oracle.** Record a four-AI game to tick T+N, and replay it to tick T.

- **Path A:** continue the replay in-process to T+N, recording every tick's checksum parts.
- **Path B:** snapshot at T, restore the snapshot in a fresh process, and continue the
  replay's commands to T+N.
- **The check:** A's and B's checksum trails must be identical for every tick. Their
  `--entity-trace` must also match, which localizes a missed field to its entity and
  subsystem.

This runs at several T values (early, mid, late, and on a forced-collection tick), and in CI
on a short game.

**Other checks:**

- **Idempotence:** for a heap without address-keyed tables, save, load, then save again gives
  the same bytes. A real heap has such tables (the registry's light userdata, sets of units),
  and their order after a load renumbers the objects after them. So the data test checks:
  - the loaded heap saves to as many bytes;
  - it iterates its value-keyed tables and reads its values as the sim's did;
  - a load of that save does too.
- **Unit tests** (`tests/test_lua_persist.cpp`, `tests/test_lua_gc.cpp`):
  - Lua round trips: layouts and iteration order; sharing and cycles; upvalues, open and
    closed; weak tables; suspended threads; C closures; light userdata; registry refs.
  - The collector: modes kept, the old heap freed, frozen tables, a save mid-sweep, and a
    state that collects only when told.
  - Each C++ serializer's round trip (M208c-b).
- **Data test** (`--persist-test`): the sim's whole heap, at load and 300 ticks in with an AI
  playing. It was 29.5 MB at load and 33 MB 300 ticks in; in Debug that took about 0.6 s to
  save and 0.1 s to load.
- **Measured:** save and load time and size for the 30- and 60-minute games of M208b.

## Slices

1. **M208c-a, Lua persistence.** `lpersist.cpp`, light-userdata hooks, frozen tables and the
   lazy sweep, and the sim collecting only when forced. Tested by unit round trips, and by
   the sim's whole heap round-tripping in `--persist-test`.
2. **M208c-b, the C++ sim.** Serializers, pointer translation, and the restore sequence.
   Proved by the A/B oracle through test flags (`--snapshot-at T`, `--restore <file>`).
3. **M208c-c, saves.** The snapshot in the save file, the load with its check and fallback,
   the UI flow, and measurements.

## Risks

- **A missed field.** It's a silent desync, and the oracle is the defence: a missed field
  diverges some later tick. Every new sim field must join a serializer. A check in the
  oracle's CI job catches omissions before release.
- **Object-keyed iteration.** If a script's result depends on it, a restored game diverges
  where a cross-OS replay would diverge too. That's the same class of bug, found the same way.
- **Heap size.** The late game's heap has about 460,000 tables, half of them blueprints that
  aren't saved. With compression, a save should stay in the tens of megabytes. M208c-c
  measures it.
- **Engine versions.** A snapshot is tied to its build, like the save's history gate: C
  functions are offsets into it. A mismatch falls back to catching up.
- **A damaged or crafted snapshot.** The loader bounds every size by the input left and checks
  every reference's kind, and a hash over the snapshot fails a corrupted one before anything
  is made. A unit test loads hundreds of damaged snapshots. A *crafted* snapshot is another
  matter: its C function offsets could name any address in the binary. Before M208c-c loads
  snapshots from save files, which players share, a load must check each offset against the
  C functions this build registers, and fail otherwise.
- **Loading still collects on Lua's threshold.** Manual collection starts at tick 1, so the
  collections during a load fall where the byte count puts them. A restore replaces the heap
  whole, so that doesn't reach snapshots. Across OSes it is an existing, untested case.
