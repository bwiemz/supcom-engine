# M188: Audio parity with retail FA

**Goal:** retail FA sounds like retail FA on the engine: the interface, units, weapons, music and voice-over. The mix is driven by FA's own XACT data, and the sound API behaves the way retail's scripts expect.

## Where it stands (2026-09-23)

- `audio::SoundManager` plays cues through miniaudio. `SimState` owns it, and only the sim Lua state can reach it. The front end has no audio at all, and the UI state finds no manager.
- **The sound-bank parser is a heuristic.** For each sound it takes the first `0xFF` separator byte as a play-wave event. That means:
  - one wave per cue;
  - no track variations (random footsteps, weapon variants);
  - no pitch or volume variation;
  - no loop counts.
- **Banks are paired by file name** (`X.xsb` with `X.xwb`). That is wrong for 19 of retail's sound banks, which play waves from other banks or from several:
  - `Interface` draws on five wave banks, including `Music` and the faction `*Select` banks;
  - `XAA` plays only `UAA`'s waves;
  - a wave bank's file name need not be its name: `XAS_Weapons.xwb` is internally `XAS_Weapon`.
- **Nothing reads `SupCom.xgs`.** So there are no sound categories, no per-category volumes or instance limits, and no distance or zoom falloff curves. A fixed inverse-distance falloff stands in for them.
- **Lua API gaps:**
  - `SetVolume`/`GetVolume` are stubs.
  - The unit-sound methods (`PlayUnitSound`, `PlayUnitAmbientSound`) are no-ops.
  - Handles returned by `PlaySound` are not waitable. Retail's music script `WaitFor`s them.

## What retail's data is (measured on FA 3599)

| File | Format | Notes |
|---|---|---|
| `sounds/*.xwb` (77) | XACT wave bank, header version 43 | All PCM, 1 or 2 channels, 24/32/44.1 kHz. 11 are *streaming* banks (Music, FMV_BG, the `*Destroy` banks, `*Stream`), about 1-40 MB each. |
| `sounds/*.xsb` (80) | XACT 3.0 sound bank (content 43, tool 43) | 1,896 cues, each a single sound: no cue-level variation tables. Variation lives in tracks (event types 3 and 6: random, no-repeat and shuffle over weighted waves). Effect variation (type 4 and 6) gives per-play pitch and volume ranges. Loop counts are 0, 1, 4 or 255 (infinite). |
| `sounds/SupCom.xgs` | XACT global settings (content 43, tool 42) | **39 categories** in a tree (Global → Music, World → Units → UnitsUEF…, Interface, VO → per-language). Each has a volume, an instance limit and behaviour, and fades. **22 variables:** `Distance`, `CameraDistance`, `ZoomPercent`, `Angle`, `Duck`, and LOD cutoffs such as `Weapon_LodCutoff`. **20 RPC curves** map a variable to volume in millibels (−9600 is silence); sounds reference them by offset. |

The XACT 3.0 layout differs from what FAudio documents (it documents XACT 3.1 and later):
- An effect-variation block is 7 bytes (pitch s16×2, volume u8×2, flags u8), with no filter floats.
- A track header is 5 bytes, with no filter data.
- A sound's events live inside its entry, so the entry length delimits sounds.

This was verified by a prototype parser that resolves all 1,896 retail cues to existing waves.

## Design

### Data layer (`src/audio/xact/`)
- `GlobalSettings` (XGS): categories (name, parent, volume in dB, instance limit and behaviour, fade in/out), variables (name, initial value, range, and whether global or per-cue), and RPC curves (variable → parameter; points are linear; y is in millibels for volume).
- `SoundBank` (XSB): the bank name; wave-bank names by index; cues (name → sound, fades, instance limit); sounds (category, volume, pitch, priority, RPC curve refs, tracks); tracks (volume, events); events:
  - **play** — a single wave, or a weighted set with a variation mode;
  - **loop count**;
  - **pitch/volume variation**.
- `WaveBank` (XWB): reads only the header and entry table. It reads a wave's bytes on demand, so the 40 MB streaming banks are never read whole. Waves are cached per bank with an LRU limit (the in-memory banks total about 60 MB).
- `BankRegistry`: indexes every `.xwb` in `sounds/` by its *internal* name. A sound bank's wave-bank references resolve through it. It finds sound banks by file stem, which is how `Sound{Bank=…}` names them.

Parsers take byte spans and return `Result<T>`. They validate every offset against the buffer, since a malformed bank must fail to load, never read out of bounds. Unit tests build small banks from bytes, so they run in CI without game data.

### Playback layer (`AudioEngine`, replacing `SoundManager`)
- **Owned by the application, not the sim.** It is created at startup (before the front end) and registered in every Lua state that plays sound. Sim teardown no longer destroys it.
- **A cue instance** follows the XACT rules:
  - It picks a sound and waves by the track's variation mode, with its own per-instance random generator.
  - It applies the effect-variation pitch and volume.
  - Loop counts are honoured.
  - An instance ends when all of its tracks end, or on stop. A non-immediate stop fades out over the cue's or the category's fade time.
- **Volume** = the sound volume × the track volume × the category-chain volumes × the user's category volume × the RPC curves (Distance per instance; CameraDistance, ZoomPercent, Angle and Duck as globals).
- **3D:** the listener is the camera. The per-instance `Distance` feeds the sound's RPC curves, as in XACT, and miniaudio pans only (its own attenuation is off). A `LodCutoff` variable culls a sound further than its value from the listener.
- **Instance limits** apply per category and per cue, with the limit behaviour (fail, queue, replace oldest or quietest).
- **Voice:** `PlayVoice` plays on the VO category and raises `Duck` while it plays. The RPC on `Duck` lowers the rest of the mix.

### Lua surface
- `Sound{Bank, Cue, LodCutoff}` → a table, as now.
- `PlaySound(sound)` → a handle object that `WaitFor(handle)` accepts: it returns when the sound ends. `StopSound(handle[, immediate])`.
- `SetVolume(category, v)` and `GetVolume(category)`. Retail's volume options call them from their `set` functions, and those options persist in preferences through M187c:

  | Option | Category |
  |---|---|
  | `master_volume` | `Global` |
  | `fx_volume` | `World` and `Interface` |
  | `music_volume` | `Music` |
  | `vo_volume` | `VO` |

- `PlayVoice(sound, duck)` and `StopAllSounds()`.
- `DisableWorldSounds()`/`EnableWorldSounds()` mute the World category (retail uses these for movies).
- Sim: `PlayUnitSound(name)` and `PlayUnitAmbientSound(name)` look up the blueprint's `Audio[name]`. `StopUnitAmbientSound(name)`. Ambient loops follow their entity's position.
- `UserMusic.NotifyBattle` is fed by the sim's battle events, if retail does not already feed it through Sync.

### Headless and tests
Without an audio device (CI, headless tests), the engine runs the same logic without output: cue instances, timing, fades, limits and `WaitFor` all work, with only the miniaudio output skipped. That lets the data and integration tests assert audio behaviour, for example "the music cue is playing" or "a second cue in a limit-1 category replaced the first".

## Milestones
| # | Scope | Exit |
|---|---|---|
| M188a | XACT data layer: XGS, full XSB, on-demand XWB, and registry by internal name. | Unit tests on synthetic banks. `--audio-data-test` (gate): every retail cue resolves to playable waves; categories, variables and RPC curves load. |
| M188b | `AudioEngine`: app-owned; categories and volumes; RPC distance and zoom falloff; variations, loops and fades; instance limits; waitable handles; `SetVolume`/`GetVolume`; the VO duck. | Front-end interface sounds play. Retail's music thread plays and cycles music. Headless tests cover the fades, limits and `WaitFor`. |
| M188c | World sounds: unit, weapon and projectile audio from blueprints; ambient loops; LOD cutoffs; world-sound enable/disable. | The 4-AI skirmish plays unit and weapon sounds within category limits, with no errors. A windowed game is audible (manual check). |
