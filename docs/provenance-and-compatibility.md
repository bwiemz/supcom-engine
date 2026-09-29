# Provenance and Compatibility Policy

OpenSupCom reimplements Moho, the engine of *Supreme Commander: Forged Alliance*, so that the
game's own data and scripts run on new code. This document says what may go into the repository
and where it may come from (provenance), and what the engine promises to run and interoperate
with (compatibility). `CONTRIBUTING.md` summarises both; this is the full statement.

## Provenance

### What the repository holds

- **Original code**, under the MIT License (`LICENSE`).
- **Third-party code** in `third_party/`, each part under its own license, whose notice it
  keeps:
  - Lua 5.0 (Tecgraf/PUC-Rio's MIT-style license, at the end of `lua.h`). It carries
    OpenSupCom changes, each marked in a comment: the LuaPlus opcodes and per-type
    metatables, C++ error unwinding, the collector's changes (M224g, M208c), and `lpersist`.
  - fdlibm (Sun's permissive notice, in each file).
  - miniaudio (public domain or MIT-0), pl_mpeg (MIT) and stb (public domain or MIT), each in
    its header.
- **Dependencies from vcpkg** (`vcpkg.json`). They are fetched at build time, not vendored,
  under their own licenses.
- **Names, not content, from the game.** Some tests and tools name things the game has: the
  retail API still unbound (`tests/integration/binding_baseline_retail.txt`), map and
  blueprint ids a test plays, cue names. Names identify; they carry none of the game's work.

### What it never holds

- **Game data of any kind.** FA's assets are proprietary: archives, extracted files, Lua
  scripts, shaders, meshes, textures, sounds, movies, maps. Tests that need the game find the
  player's installation, and skip (exit 77) without it.
- **Anything made from game data.** That covers screenshots or golden images showing game
  art, recorded sounds, captured traces holding game text, and snapshots or saves. Goldens
  and benchmark baselines live outside the repository (`$OSC_GOLDEN_DIR`, else
  `<State>/opensupcom/golden`).
- **Code or text copied from the game or its engine.** That covers retail or FAF Lua,
  HLSL from FA's `.fx` files, and anything decompiled.

### Where knowledge may come from

Reimplementing an engine means learning what it does. These sources are used for that, and
only for that: the engine's code is written anew.

| Source | Used for | Never |
|---|---|---|
| FA's and FAF's Lua scripts | What the engine must provide: which bindings a script calls, with what arguments, expecting what back | Copying script code into the engine, or shipping modified game scripts |
| FA's `.fx` shaders | The formulas a surface is drawn with (lighting, the water's layers, the strata blend). The engine's GLSL states the maths in its own code | Copying HLSL text or comments |
| The decompiled engine ([faf-re](https://github.com/Draiget/faf-re), FAF's patched `ForgedAlliance.exe`) | Moho's rules and their order where scripts leave them unclear: defaults, timings, which task runs first. Design notes and memories cite it by file | Copying decompiled code, even translated. A rule learned from it is re-expressed and tested |
| FAF's engine annotations and community documentation | API shapes and argument orders | Wholesale copying of their text |
| Running the game | Observed behaviour, on the developer's own copy | Committing what was captured |

When a source leaves a rule unclear, the design note says so and says what was chosen. Examples
are the right-click order per target (`moho-default-orders`), and the XACT 3.0 variation flags
worked out from the retail bytes.

### Contributions

Every contribution follows these rules, whether written by a person or with an AI assistant.
Reviewers ask where a non-obvious rule came from. The design notes in `docs/plans/` record it,
so a later reader can check the reasoning without the game's code.

## Compatibility

### The data the engine runs

- **Retail FA 3599** (Steam app 9420) is the reference. CI's data-free tests and the local
  gate (`ctest -L gate`) are measured against it. An installation is found through Steam,
  FAF's data directory, the environment or a folder the player chooses (M228).
- **FAForever's data** (`--faf-data`, `~/.faforever`) must keep working. It isn't the gate:
  this project's development machine has no FAF install. A change that breaks FAF is a bug.
- **Mods** work as Moho's do: `/schook` hooks, sim and UI mods, a game's mods reaching its
  states before their blueprints load, and zipped mods (M221). A mod that relies on FAF's
  engine patches is supported only where the engine implements what the patch did.

### How faithfully

- **Match Moho as far as scripts can observe.** A binding returns what Moho's returns, in the
  order and on the tick Moho's does. Retail's scripts are the specification. Where they
  disagree with FAF's, retail wins and FAF must still run.
- **Scripts decide, the engine provides.** When a script implements a rule (victory, score,
  AI, UI, wrecks), the engine supplies Moho's primitives and doesn't decide the outcome.
- **The game's own bugs are kept.** They are part of how the game plays. For example,
  retail's AI grows its base-monitor points without bound through a Lua `continue` bug, as it
  does in Moho, and the engine doesn't fix it. An engine bug a script depends on is matched
  when the dependence is known and documented.
- **The engine may differ where no script can tell.** It may be faster, 64-bit, on
  another OS, or more deterministic:
  - collections that sweep lazily (M224g);
  - collections that run only on the sim's schedule (M208c);
  - a sim that walks entities in id order (M195) and draws all randomness from one stream
    (M196).

  Such a difference is documented where it is made.

### Files it reads and writes

- **Reads FA's formats:** `.scd` and `.nx2` archives, `.bp` and `.lua`, `.scmap` (every
  retail map to its last byte), SCM meshes and SCA animations, DDS textures, XACT 3.0 banks,
  and Sofdec movies.
- **Writes formats of its own**, never FA's:
  - replays (`.oscreplay`) and saved games (`.oscsave`), in the folders retail's dialogs
    list;
  - preferences, in retail's `Game.prefs` Lua form (so its profiles and options carry
    over).
- **Retail's `.SCFAReplay` and retail saves aren't read.** Original-replay compatibility is
  out of scope until 1.0.
- **Replays and saves are tied to the build that wrote them.** A different build refuses them
  (`WrongVersion`), since only that build is known to replay them as they were played.

### Multiplayer and services

- **Lockstep games need the same build on every peer.** They must also have the same data
  and mods. Windows and Linux play together (`cross-os-play` in CI; `tools/cross_os_replay.py`
  on real data).
- **The LAN lobby** is retail's `lobby.lua` on the engine's `CLobby`. It speaks OpenSupCom's
  wire format, so it plays with OpenSupCom peers, not with retail executables.
- **FAF's client** can launch the engine over GPGNet, and its ICE adapter can relay a game
  (M220). FAF's own lobby scripts and its servers' rating rules aren't a compatibility
  promise until M220d.

### Platforms

- **Supported:** Windows (MSVC) and Linux (GCC or Clang) on x86-64. CI builds and tests both.
  Release packages are a Windows zip and a Linux AppImage (M227).
- **Not yet:** macOS, and other architectures.

## Changing this policy

A change to this document is a change of promise. Make it in its own PR that says why, and
update `CONTRIBUTING.md`'s summary with it.
