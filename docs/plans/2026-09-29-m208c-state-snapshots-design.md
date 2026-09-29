# M208c: State Snapshots — Design

**Status:** 2026-09-29. Implemented: M208c-a (Lua persistence), M208c-b (the C++ sim,
headless loads) and M208c-c (the game's own load, its fallback, signed snapshots).
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
  a (kind, id) pair and back again (see below). NULL stays NULL. The persister fails a save
  on an address its hooks can't name. The sim's hooks name every address: a stale one as
  NULL, counted in the log. Host singletons (`osc_sim_state`, `osc_thread_mgr`, …) are
  re-pointed on load.
- **Full userdata:** their bytes as they are. The exception is the io library's files, the
  only full userdata the sim holds, which are written as the standard stream they are.
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

The serializers are `StateIO`'s (`src/sim/state_io*.cpp`), which each serialized class
befriends. They use their own writer and reader, whose section tags fail a misaligned read
where it happens. The replay codec drops the runtime fields of queued orders. Each field is
written in declaration order; the snapshot as a whole carries a version and a hash.

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
- **Constants from blueprints are saved too.** A unit's drive, threats and transport layout
  are saved rather than re-read. Re-reading them would mean replaying unit creation, which
  calls `luaL_ref` and runs scripts. Only caches are rebuilt: bone and animation data,
  projectile blueprint info, placement rules.
- **Forgotten fields are caught.** `tools/check_state_io.py` (the `arch.state_io` test) reads
  every serialized class's members from its header. It fails on any the serializers never
  name, in code or in a comment that leaves the field out with its reason.

**Pointer translation** (`src/sim/sim_snapshot.cpp`):

- **Naming.** Lua's `_c_object` values name C++ objects by address. A save names each address
  by what it is: an entity (id), a weapon (unit, index), a navigator (unit), a manipulator
  (unit, index), an economy event (index), a brain (army) or a platoon (army, index). A load
  finds the object that name is in the restored sim.
- **Host singletons.** These are the sim, its thread manager, the VFS, the blueprint store,
  the sound manager and the game-state manager. They are named by the registry or global
  key that holds them, and a load takes the booted host's pointer under that key.
- **Stale handles.** An address that names nothing live becomes NULL. A navigator table
  outlives its unit, because nothing detaches it.
- **After the load:**
  - `_c_sim_gen` fields are rewritten to the new generation;
  - each thread's coroutine is found again from its registry ref;
  - the booted blueprint store's refs must equal the saved game's, since blueprint tables
    are named by ref.
- **The io library's files.** Their `FILE*` belongs to the saving process, so a file handle
  is written as the standard stream it is. An open file fails the save.
- **The old heap's finalizers don't run.** They would run the old heap's code against the
  new registry.

### 3. The save file and the load

- **The save file.** `SavedGame` (format 2) carries its snapshot, deflated with zlib at
  fastest, beside the history. A save without one still loads by catching up. Saves only
  load on the build that wrote them, so there are no older ones.
- **Saving.** Snapshots are taken at a tick boundary, never inside one. The sim writes one
  when the UI's save request is processed; `InternalSaveGame` doesn't change.
- **The restored game adopts the save's history as its recording** (`adopt_history`), so a
  later save carries the whole game. The history drops the orders still to run: they're in
  the restored scheduler, and are recorded again as they run.
- **Every load restores** (headless since M208c-b, the Load dialog's relaunch since M208c-c).
  `--load-by-replay` forces the catch-up, the oracle a restore is checked against.
- **Only this installation's snapshots are restored** (M208c-c). A snapshot names C
  functions by their place in the binary, so a crafted one could aim a call anywhere.
  - Each save signs its snapshot with HMAC-SHA256. The key comes from the OS's cryptographic
    random source (getentropy, BCryptGenRandom), and is kept in the user folder
    (`opensupcom-snapshot.key`) in a file only its owner can read (0600 on POSIX).
  - A load restores only a snapshot its own key signed. Any other save, such as one a
    player was sent, catches up from its history. That runs only the local game's scripts
    and the recorded orders, as a replay does.
  - A list of the C functions a build can register was measured and rejected. A late game's
    heap holds functions its freshly booted one doesn't: the AI personality's lambdas and
    the falling-tree motor, bound on first use.
- **A restore that fails** reboots the game and catches up, in the game's own load. "Fails"
  means a damaged snapshot, or one that isn't the saved game by its checksum. A headless
  load exits instead, since it is a test.
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

**What M208c-b showed** (Release, Seton's Clutch, four AIs, seed 4242, `--scripted-orders`):

- **Save points.** Saves at ticks 70 (a forced collection), 71 (its lazy sweep under way),
  1402, 3000, 6000, 12000 and 17000 each played 500 ticks identically after a restore, in
  every checksum domain.
- **Entity traces.** After a restore at 600, the traces for ticks 601–700 are byte-identical:
  571,658 lines each.
- **The campaign.** X1CA_001, restored at tick 900, played its next 500 ticks identically.
- **The ctest tests.**
  - `data.save_load` loads a save, saves again in the loaded game, and loads that second
    save.
  - `data.save_load_replay` runs the same chain catching up, and reaches the same final
    checksums.
  - `data.save_load_campaign` runs the chain on the campaign.
  - A unit test restores a sim without a map, so CI covers the snapshot too.
- **Size and time.** At tick 18000 (30 minutes), the snapshot is 77 MB and takes 456 ms. The
  file is 11.9 MB and is written in 683 ms. A restore takes 253 ms after the boot, where
  catching up took about 100 s of sim.
- **Scripted orders.** `--scripted-orders` now seeds its orders from the tick, so a restored
  game is given the ones the saved game was. Headless runs go to `--ticks`' tick, not that
  many ticks more.

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
2. **M208c-b, the C++ sim.** Serializers, pointer translation, the restore sequence, the
   snapshot in the save file, and headless loads that restore it. Proved by the A/B oracle:
   saves loaded both ways (`--load`, `--load-by-replay`).
3. **M208c-c, the game's loads.**
   - The Load dialog's relaunch restores.
   - A failed restore reboots and catches up.
   - Snapshots are signed by their installation, and only its own are restored.
   - Proved by `data.load_flow`:
     - three loads restore;
     - the same flow on a save whose snapshot was damaged under a valid signature
       falls back and passes;
     - `data.save_load`'s fourth process, another installation, catches up to the same
       game.
   - Left for later:
     - ambient sound loops restart only when their scripts next play them (M216b);
     - the focus army in the sim's registry is the saving client's, which a
       single-player load shares.

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
- **A damaged or crafted snapshot.**
  - The loader bounds every size by the input left and checks every reference's kind.
  - A hash over the snapshot fails a corrupted one before anything is made. A unit test
    loads hundreds of damaged snapshots.
  - A *crafted* snapshot could name any address in the binary as a C function. So since
    M208c-c only a snapshot signed with the loading installation's own key is restored,
    and any other save catches up.
  - The key is only as safe as the user folder it's kept in.
- **Loading still collects on Lua's threshold.** Manual collection starts at tick 1, so the
  collections during a load fall where the byte count puts them. A restore replaces the heap
  whole, so that doesn't reach snapshots. Across OSes it is an existing, untested case.
