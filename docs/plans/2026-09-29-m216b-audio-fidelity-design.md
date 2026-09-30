# M216b: audio fidelity — gap analysis and plan

Date: 2026-09-29.

**Sources.** Engine paths are relative to the repo. faf-re paths (the decompiled Moho) are relative to its `src/sdk/moho/`. FAudio (`FNA-XNA/FAudio`, the open XACT reimplementation) is cited by file and line. The retail counts come from a scan of every retail `sounds/*.xsb` (80) and `sounds/Voice/US/*.xsb` (10) and every `.xwb`, with a parser mirroring the engine's; the scan scripts are not in the repository (they read game data). The checks that matter are now data tests (`--audio-data-test`).

**Done in the first M216b change:** items 1–3 of the plan below — the 3.0 effect-variation flags (0x40 pitch, 0x80 volume, and a one-value range as a fixed offset), the voice banks (`AudioSetLanguage`, `HasLocalizedVO`), and a new wave per loop for Music's two game tracks (with no-immediate-repeat as an exclusion). `PlayTutorialVO` stays unbound: no retail script calls it.

**Done in the second change:** items 4–7 and 10 — category pause (`PauseSound`/`PauseVoice`, subtree, clocks held), ambient slots play their cue as authored (no forced loop), fades only on replacement with the limit's own fade-in/out, `stop()` without the category fade, the duck ramp over `DuckLength` (reset by `SetVolume` and `StopAllSounds`), `/nomusic`, and `StopAllSounds` as a released stop. Moho's natural-end duck pop stays a deliberate divergence (open question 4).

**Done in the third change:** item 8 and the listener and panning half of item 9. The listener stands at Moho's point (the focus raised by the target zoom, less 4), facing the view. Each positional cue's `Angle` is its own: degrees off straight up, reading faf-re's `ComputeCueAngleDegrees` in Moho's Y-up world. That settles open question 3: retail's curve is −23 mB straight below and −2017 mB level, which only fits the vertical-angle reading. World sounds get X3DAudio's linear stereo matrix, through miniaudio's balance pan and the louder side's gain, since miniaudio's no-attenuation model doesn't pan at all. Still open in item 9: loop positions from the interpolated frame, and 5.1.

**Done in the fourth change:** item 12, and item 9's loop positions. Script sounds are sim state: one-shots are the tick's `SoundRequest`s, and an entity's loops are the ones it wants, saved with the game. The audio side filters them once a frame, as `CUserSoundManager::UpdateSoundRequests` does. A one-shot is dropped beyond its LodCutoff against CameraDistance, out of the player's army's sight (or an ally's), or when the same cue already played that beat. An entity loop starts only for an entity in the world camera's frustum, with CameraDistance at most 200. It is released out of sight, stopped at once beyond its cutoff, restarted after its cue ends, and follows the drawn (interpolated) entity. `DisableWorldSounds` gates requests instead of muting. Underwater sounds use the same sight as the rest. Moho's 'Fog' mask there is its water-vision grid; the engine paints WaterVision into its one Vision grid, so surface sight counts too. Before this change the sim had called the sound engine directly, and `PlayUnitSound`/`PlayUnitAmbientSound` had answered Lua differently with and without an audio device.

**Done in the fifth change:** item 11. A PCM wave of 2 MB or more (retail's music, 34 MB, and movie voices, up to 43 MB) streams from its bank. miniaudio's decoder reads a WAV whose header is in memory and whose samples are read from the bank's file as they play, byte for byte the WAV the short path builds in memory. Starting a track no longer reads and copies tens of megabytes on the main thread, and a headless run never reads one. Items 13 (ADX) and 14 (`RPCSound`) stay deferred; the listening pass remains.

## Headline findings (ordered by what a player hears)

1. **All voice is silent.** `BankRegistry` reads only the top level of `sounds/` (`src/audio/xact/bank_registry.cpp:31`). `sounds/Voice/US/` holds 10 sound banks and 1,275 cues:
   - `XGG`: in-game EVA and unit VO, 578 references in `vo_xgg.lua`;
   - `X01..X06_VO` and `X1T_VO`: campaign VO;
   - `Briefings`;
   - `X_FMV`: the voice tracks of the intro, outro and credits movies.

   `AudioSetLanguage` and `HasLocalizedVO` are stubs (`src/lua/moho_bindings.cpp:4265-4267,4542`; `src/app/boot.cpp:440-442`). Moho builds a separate VO engine from `/sounds/voice/<lang>` and a tutorial engine from `.../tutorials` (`sim/Sim.cpp:26046-26056`).
2. **The effect-variation flags are swapped.** The engine reads 0x80 as pitch and 0x40 as volume (`src/audio/xact/sound_bank.cpp:51-52`). The retail data proves the opposite: **0x40 = pitch, 0x80 = volume**, which is also FAudio's `variation_flags_from_3_0` (`FACT_internal.c:2260`).
   - On all 129 events with only 0x40 set, the volume range is the XACT tool's default, bytes 151..214 (−302..+299 mB). Their pitch ranges vary (−200..200, −300..0, fixed +800, +900, +1200...).
   - On all 100 events with only 0x80 set, the pitch range is the tool's default (−100..100 cents). Their volume ranges vary (143..180, 160..180, 191..191...).
   - So 229 events are wrong:
     - the UEF and Aeon air units (UEA 63, UAA 52) lose their authored pitch spread and gain a ±3 dB wobble;
     - the land units (UEL, UAL, URL, UEB...) lose their volume spread and gain a ±1 semitone wobble;
     - UI sounds built as pitched stacks play in unison instead: `UI_Menu_Rollover` has tracks at +800 and +1200 cents; `UI_Comm_UEF_Out`, `UI_Comm_SER_In/Out` and `UI_AEON_Rollover` are the same kind.
   - The fixture and unit test encode the wrong mapping: `tests/xact_fixtures.hpp:130`, `tests/test_xact.cpp:113`.
   - Memory `audio.md` states this wrong ("0x80 is pitch") and should be corrected.
3. **Music never changes track.**
   - `Music/Base_Building` (5 waves) and `Music/Battle` (4 waves) are single loop-forever events. Their variation word has bit 0x40 set (info>>16 = 0x43): "new variation on loop". They are the only 2 such events in the data.
   - The engine ignores that bit: `read_track_variation` keeps only `& 0x7` (`sound_bank.cpp:65`). It then loops the first-picked wave forever through `ma_sound_set_looping` (`sound_manager.cpp:575`). FAudio re-picks on every loop (`FACT_internal.c:193-271`).
4. **Pausing the game doesn't pause sound.**
   - Retail `gamemain.OnPause`/`OnResume` call `PauseSound("World"/"Music")` and `PauseVoice("VO")` (lua `ui/game/gamemain.lua:386-397`). So do the operation briefing (`operationbriefing.lua:370-384`) and transmissions (`missiontext.lua:181`).
   - The engine's bindings are no-ops (`moho_bindings.cpp:3765`).
   - Moho pauses the category on the voice or VO engine (`audio/CUserSoundManager.cpp:2398,2462`). XACT pauses the category's whole subtree (FAudio `FACTAudioEngine_Pause`, `FACT.c:894-923`).
5. **World sounds are neither panned nor attenuated like Moho's.**
   - The engine sets `ma_attenuation_model_none` "so miniaudio only pans" (`sound_manager.cpp:572-573`). In miniaudio 0.11.24 the `none` model takes an early path that **skips panning entirely**: it only channel-maps mono to [1,1] (`third_party/miniaudio/miniaudio.h:52329-52346`).
   - Moho runs X3DAudio per cue with a flat distance curve, flags 0x61 = matrix, doppler and emitter angle (`audio/AudioEngine.cpp:192,210-215,2736-2766`). It applies the matrix and the per-cue `Distance`, `DopplerPitchScalar` and `OrientationAngle` (`AudioEngine.cpp:1225-1258`).
   - X3DAudio's stereo matrix, per F3DAudio (FAudio's X3DAudio reimplementation, fitted to X3DAudio's own results; `F3DAudio.c:585-641,1030-1072`), pans linearly between speakers at ±90° with L+R = 1. A centred sound gets [0.5, 0.5].
   - So engine world sounds are unpanned and up to **+6 dB** louder than in retail, relative to 2D UI, music and VO. XAudio2's 2D mono→stereo default is [1, 1] (`matrix_defaults.inl:69`), which the engine matches.
6. **The per-cue `Angle` RPC is a constant −4.4 dB on 1,728 world sounds.**
   - `Angle` is cue-scoped: XGS accessibility 5 = public|cue.
   - The engine sets it as a global from the camera pitch (`src/app/window.cpp:463`). `set_global_variable` drops non-global variables (`sound_manager.cpp:707`), so every cue reads the XGS initial value of 32.64°. RPC 1437 gives −438 mB there.
   - Moho sets `Angle` per cue from the emitter–listener vector, for one-shots and entity loops (`CUserSoundManager.cpp:875-883,1445-1449,391`).
7. **Moho's world-sound filtering is missing** (CUserSoundManager, `UpdateSoundRequests`):
   - an LOS cull against the focus army: sounds in the fog are inaudible in retail, audible in the engine (a fairness leak);
   - one one-shot per (bank, cue) per beat;
   - entity loops start only for entities in the camera frustum, and only when CameraDistance ≤ 200.

   In the engine, off-screen loops use up the 60-slot `Units*` categories (limit behaviour Fail), so on-screen units go silent in big games.
8. **Smaller audible differences:**
   - The duck steps where Moho ramps over 0.5 s.
   - The `Ambient` category fades every play in over 1 s: `UI_Camera_Save/Recall/Delete_Position`, `Gen_Fire_Start`, `Gen_Tree_Crush`.
   - 14 unit blueprints put a one-shot cue in an ambient slot. The engine forces it to loop forever: 13 `StartMoveSub` subs, plus `UEL0203` water movement.
   - Replacing a music cue crossfades over 6 s (the release) instead of 200 ms.
   - The score screen's `StopAllSounds` cuts dead instead of releasing.
   - Starting a music or movie-VO wave loads 34–43 MB synchronously.

## Retail data facts (measured)

| Item | Count / value |
|---|---|
| Sound banks | 80 top-level + 10 in `Voice/US`. Cues: 1,896 + 1,275 (VO). Sounds: 3,180. |
| Wave banks | 89 (incl. `Voice/US`), 3,043 waves. **All PCM** 16-bit (32k/44.1k/24k, mono 3,011, stereo 32). 22 streaming. **0 waves with a loop region.** 0 compact. |
| Event types | PlayWave 110; TrackVariation 5; EffectVariation 1,562; Track+Effect 119. **No** Stop/Pitch/Volume/Marker events. |
| Loop counts | 0: 1,409; 255: 385; 1: 1; 4: 1. |
| Track variation | 124 events, **all** type 3 (RandomNoImmediateRepeat), table type 0 (wave), **all equal weights**. 2–6 waves (376 waves in all). "New variation on loop" (0x40): 2 events (Music `Base_Building`, `Battle`). |
| Effect variation flags | 0xC0: 1,452. 0x40: 129. 0x80: 100. **No** add-mode (0x01/0x04) or new-on-loop (0x10/0x20) bits. |
| Sound flags | 0x00 1,374; 0x01 54; 0x02 70; 0x03 1,297; 0x05 3; 0x07 382. **No DSP/reverb (0x10).** 219 sounds with a base pitch. |
| Play-wave flags / pan | 0x0C: 1,794; 0x0D: 2. Position 0 / angle 36,000 on 1,483 events (a 2D pan; overridden for 3D cues). |
| Cue variation tables, transitions | **None** (no interactive music). |
| Cue limits | 1,879 limited: Fail 1,805, ReplaceOldest 62, ReplaceLowestPriority 12. No Queue or ReplaceQuietest. |
| Cue fades | Fade-out on 59 cues: `Unit Select` 53 × 250 ms, `UnitsUEF` 4 × 250, `Music` 2 × 200. **Fade-in on 0 cues.** |
| XGS | 39 categories, 22 variables, 20 RPCs, 0 DSP presets. **Every RPC drives Volume (parameter 0), all points linear.** |
| Movies | 1,004 `.sfd`. **Only `e3_demo_cut.sfd` has an audio stream** (ADX, stereo, 48 kHz, 18-byte frames), and no Lua references it. |

XGS categories that matter (volume in mB; limit/behaviour; fade in/out ms): Global +601; Default −202; Music +1 (1, ReplaceOldest, crossfade type 1, 0/200); World +200 (200, Fail, 0/100); Units +1; Ambient −202 (fade 1000/1000); Weapons +1 (150); Destroy +103 (100); Interface −495; `Units{UEF,AEON,CYBRAN}` −202 (60); `UnitsUEFAir` −802; `ActiveLoops*` −600/−495 (60); `Construction_Loops` −302 (60); `ConstructionSeraphim` −495 (30); FMV −396; VO +1; US −574.

XGS variables:

| Variable | Scope | Initial |
|---|---|---|
| NumCueInstances, AttackTime, ReleaseTime | cue, reserved | 0 |
| OrientationAngle, DopplerPitchScalar, Distance | cue, reserved | 0, 1, 0 |
| SpeedOfSound | global, reserved | 343.5 |
| `*_LodCutoff` | global | Weapon 10000, UnitMove 10000, WeaponBig 10000, Default −1, UnitRumble 256 |
| Rumble1, UAL_Rumble1, UES_Rumble1, URA_Rumble1, UEL_RumbleTanks1 | global | 0 |
| CameraDistance | global | 576.8 |
| Duck | global | 0 |
| DuckLength | global | 0.5 |
| ZoomPercent | global | 75.8 |
| **Angle** | **cue** | **32.64** |

RPC curves in use (code: variable → sounds using it):

| Code | Variable | Sounds | Shape |
|---|---|---|---|
| 1355 | Duck | 1,744 | 0 → −1000 mB at 0.529 |
| 1437 | Angle | 1,735 | 0° ≈0, 45° −613, 90° −2021, 119° −462, 180° −23 |
| 868 | Distance | 1,124 | 794 u −24 dB, silent at 3,747 |
| 918 | Distance | 315 | |
| 1305 | Distance | 140 | silent at 182 u |
| 1050 | Distance | 113 | |
| 1109 | Distance | 42 | |
| 1200 | Distance | 7 | |
| 968 / 1027 | AttackTime 1.0 s / ReleaseTime 2.99 s | 225 tracks | |
| 1223 / 1282 | AttackTime 0.53 s / ReleaseTime 1.01 s | 159 tracks | |
| 1150 | ReleaseTime 6.06 s | 3 Music tracks | |
| 1378 | CameraDistance | 1 (`AMB_Planet_Rumble_zoom`) | |
| 753 | Rumble1 | 1 | |

No RPC reads ZoomPercent, DopplerPitchScalar, OrientationAngle, NumCueInstances or the four other Rumble variables. Curve 1487 (Distance) is defined but unused.

## Feature by feature

Each feature lists (a) the retail data that uses it, (b) what Moho or XACT does, (c) what the engine does today, and (d) the gap and its fix.

### 1. Effect variation: pitch and volume randomisation
- **(a)** 1,681 events: 1,452 × 0xC0, 129 × 0x40, 100 × 0x80. All replace-mode; none new-on-loop. 374 of them are looped, with the variation fixed for the whole loop.
- **(b)** FAudio `GetNextWave` (`FACT_internal.c:309-379`):
  - pitch = uniform(min, max) + sound pitch;
  - volume = uniform(min, max) + sound volume + track volume, in millibels;
  - one value per play when not new-on-loop.
- **(c)** Uniform draws in the same units (`sound_manager.cpp:553-559`). Only the flag mapping is wrong (`sound_bank.cpp:51-52`).
- **(d)** Swap to 0x40 = pitch and 0x80 = volume. Parse 0x01 (volume add), 0x04 (pitch add), 0x10 (pitch new on loop) and 0x20 (volume new on loop) too, for completeness; FA uses none. Fix the fixture and test. **Size: S.**

### 2. Track (wave) variations
- **(a)** 124 events, all RandomNoImmediateRepeat with equal weights; 2 with new-variation-on-loop (Music).
- **(b)** XACT picks per play; no-repeat excludes the previous pick (FAudio `FACT_internal.c:224-243`). FAudio keeps the state per cue instance (`evtInst->valuei`, `:740-783`). Real XACT's no-repeat across cue plays is not verifiable here; the engine's per-event state is the better guess for weapon and footstep variety. A new-variation-on-loop event re-picks on every loop (`:193-271`) and does not hand the loop to the voice (`:272-276`).
- **(c)** Per-`PlayEvent` global state. No-repeat by retrying up to 8 times (`sound_manager.cpp:301-316`): a repeat stays possible, with p = 1/256 for 2 waves. The new-on-loop bit is dropped.
- **(d)**
  - Parse bit 0x40 of `info>>16` into `PlayEvent::new_variation_on_loop`.
  - For such events: no `ma_sound_set_looping`; at each loop end, pick again and start a new voice.
  - Make no-repeat an exclusion (renormalise over the other waves).

  **Size: S.** This fixes the music cycling.

### 3. Loops and loop regions
- **(a)** 385 loop-forever events; 1 × loop 1; 1 × loop 4. **No wave has a loop region**, so loop regions are moot for retail.
- **(b)** XACT loops the whole wave (or its region) seamlessly.
- **(c)** Loop-forever uses `ma_sound_set_looping`, which is seamless. Finite loops re-seek when `update()` notices the wave ended (`sound_manager.cpp:767-773`), leaving a gap of up to one frame; this affects 2 events.
- **(d)** Optional: loop finite counts inside a data source (loop region = whole wave). The loop region is already parsed (`xwb_parser.cpp:196-197`); honour it only if mods need it. **Size: S, low value.**

### 4. Category volumes, hierarchy and user volumes (options)
- **(a)** 39 categories with authored volumes (Global +6 dB, World +2 dB, Interface −5 dB...).
- **(b)**
  - `SetVolume(cat, v)` goes to XACT `SetVolume(categoryId, v)` on the voice, VO and tutorial engines, and **also resets the duck** (`CUserSoundManager.cpp:2144-2166`; `AudioEngine.cpp:2640-2660`).
  - `GetVolume` returns the stored value, default 1 (`AudioEngine.cpp:2671-2686`).
  - Retail options (`options.lua:700-807`) map master → `Global`, fx → `World` + `Interface`, music → `Music`, vo → `VO`, each with value/100 (linear), including the mute-toggle save and restore.
  - `/nomusic` sets Music to 0 (`AudioEngine.cpp:2248-2250`).
- **(c)** gain = Π over the category chain (authored × user) (`sound_manager.cpp:329-344`). User volume is clamped to 0..1 (`:721`). SetVolume doesn't touch the duck. No `/nomusic`.
- **(d)**
  - The hierarchy is unverifiable here. FAudio multiplies only the sound's own category, plus a recursive-SetVolume quirk (`FACT.c:868-890`). Keep the chain, which matches XACT authoring semantics, and log it as an open question.
  - Add the duck reset on SetVolume and `/nomusic`.

  **Size: S.**

### 5. Instance limits
- **(a)**
  - Category limits: Units* 60, Weapons 150, Destroy 100, World 200, Construction 30/60, Music 1 (ReplaceOldest).
  - Cue limits on 1,879 cues: mostly 2–4 with Fail; 57 are limit 1 with ReplaceOldest (`Unit Select` and Music).
- **(b)** FAudio `play_sound`/`handle_instance_limit` (`FACT_internal.c:528-609,833-876`):
  - limits are checked at **Play**, not Prepare;
  - they count only the exact category, skipping stopping cues;
  - Fail refuses;
  - a replacement fades the victim out over the **limiting category's or cue's fadeOut**, not its release, and fades the new instance in over the limiter's fadeIn;
  - limit behaviour byte: high 5 bits = behaviour, low 3 bits = crossfade curve (Music and World: 1).
- **(c)**
  - `admit()` counts Playing instances of the exact category (`sound_manager.cpp:391-436`).
  - A replacement calls `stop(victim, false)`, which prefers the **release RPC** (`:423,613-632`). Music replacing music: 6.06 s instead of 200 ms.
  - Limits apply at prepare (M216a).
- **(d)**
  - On replacement, fade the victim over the limiter's fadeOut (and the new instance over its fadeIn); never use the release there.
  - Count limits at start.
  - Optionally use the crossfade curve type.

  **Size: S.**

### 6. Fades, stop and release
- **(a)** Cue fade-outs as listed above (no fade-ins). Category fades: Ambient 1000/1000, Music 0/200, World 0/100. Release RPCs on 387 tracks.
- **(b)**
  - In XACT, category and cue fade in/out are the **instance-limiting crossfade** times.
  - FAudio `FACTCue_Stop` without IMMEDIATE (`FACT.c:2376-2462`): cue fadeOut if > 0, else release RPC, else stop now. It never uses the category fade.
  - Moho `StopSound(h)` → `Stop(0)` (release); `StopSound(h, true)` → `Stop(1)` (`CUserSoundManager.cpp:2048-2076`).
- **(c)**
  - Every play fades in over the cue's or the category's fade-in (`sound_manager.cpp:470-473,590`), so Ambient cues fade in over 1 s on every play.
  - A stop tries the release first, then the cue fade, then the **category** fade (`:613-642`).
- **(d)**
  - Fade in only when replacing.
  - Drop the category fade from `stop()`.
  - The order of the cue fade and the release for a cue that has both (Music: cue fade 200 ms and release 6.06 s) is an **open question**: FAudio says 200 ms, the XACT authoring model says release. `UserMusic.StartPeaceMusic`'s `StopSound(Music)` + `WaitFor` + 3 s gap depends on it. Keep the release until it's confirmed.

  **Size: S.**

### 7. RPC variables
| Variable | Moho source | Engine (file:line) | Gap |
|---|---|---|---|
| Distance (cue) | X3DAudio emitter–listener distance, once at play for one-shots, per frame for entity loops (`AudioEngine.cpp:1240-1243`; `CUserSoundManager.cpp:1443,1825`) | per instance, recomputed each update from the listener (`sound_manager.cpp:346-352`) | Depends on the listener position (§8). One-shots re-evaluate as the camera moves; Moho freezes them at start (minor). |
| Angle (cue) | per cue, emitter vs listener (`CUserSoundManager.cpp:875-883,1445-1449`; loops `:391` + `UpdateEntityLoopSpatialization`) | never set per cue (global set dropped: `window.cpp:463`, `sound_manager.cpp:707`), so fixed at 32.64°, −438 mB | Compute per instance. The faf-re helper `ComputePitchRadians` has no binary address and uses (x,y) as horizontal and z as vertical, though Moho is Y-up. The curve (≈0 dB at 0° and 180°, −20 dB at 90°) fits "angle off the vertical": straight below ≈180°, horizon 90°. Check the axes in the binary at `0x008AA4E0` or the EntitySound path before settling. |
| CameraDistance (global) | `CameraImpl::LODMetric(focus)`, the view's current LOD metric (`CUserSoundManager.cpp:1336-1346`) | `cam.zoom()` = target zoom (`window.cpp:461`) | Same quantity at rest; target rather than current while the zoom glides. 1 sound uses it. |
| ZoomPercent (global) | targetZoom / maxZoom × 100 (`:1349-1353`) | same (`window.cpp:462`) | None, and unused by any RPC. |
| Duck / DuckLength | ramped (§11) | stepped 0/1 (`moho_bindings.cpp:3733-3736`) | §11 |
| AttackTime / ReleaseTime | XACT: ms since the track's first event / since the release began (FAudio `FACT_internal.c:1044-1063,1473-1488`) | ms since the instance started / since the stop (`sound_manager.cpp:362-366`) | Equal for events at t = 0 (all retail). |
| DopplerPitchScalar | X3DAudio doppler with zero velocities, so 1 (`AudioEngine.cpp:1246-1247,2744-2749`) | not set | No RPC reads it: no gap. |
| OrientationAngle, NumCueInstances, SpeedOfSound | set but unread | NumCueInstances computed | No gap. |
| `*_LodCutoff` | culls when **CameraDistance** > cutoff (`CUserSoundManager.cpp:2249-2254`) | culls when **emitter distance** > cutoff (`sound_manager.cpp:459-466`) | Wrong quantity. Moot in retail (cutoffs of 10000 or −1; UnitRumble's 256 is unused) but wrong for mods. Fix alongside §9. |
| Rumble* | `RPCSound` shared RPC loops publish the tracked-entity count (`CUserSoundManager.cpp:1698-1755`; `CSndParams.cpp:777`) | `RPCSound` unbound | Dead in retail: `Prop.lua:231` reads `bp.Audio.Audio.AmbientRumble`, and no data calls `RPCSound`. Defer. |

RPC parameters other than volume (pitch, filter, reverb) are **unused by FA**: nothing to build.

### 8. Listener, 3D positioning, attenuation and panning
- **(b)**
  - Listener position = (focus.x, focus.y + targetZoom, focus.z) (`CUserSoundManager.cpp:1799-1806`), then y −= 4 (`AudioEngine.cpp:2698-2701`). Orientation is the camera view's (`:2703-2720`).
  - Per cue, X3DAudio with a flat volume/LFE curve (`AudioEngine.cpp:210-215,1632-1637`), so all attenuation comes from the XACT Distance RPCs. Stereo speakers sit at ±90° with a linear pan and L+R = 1 (F3DAudio).
- **(c)**
  - The listener is the camera eye (`window.cpp:449-460`), at eye_distance = zoom / (2·tan(fov/2)) ≈ 0.79–0.87 × zoom from the focus (`renderer/camera.cpp:117`).
  - miniaudio spatialisation is on, but the `none` path does no panning (see headline 5).
- **(d)**
  - Put the listener at Moho's point and basis.
  - Compute the X3DAudio stereo matrix per voice (azimuth of the emitter projected into the listener's front/right plane; linear between ±90°; centre [0.5, 0.5]). Apply it through a 1→N gain node or balance pan plus gain. Turn off miniaudio's spatialiser.
  - Update loop positions per frame from the interpolated transform; they are 10 Hz sim positions now (`sim_state.cpp:1146-1149`).
  - 5.1: Moho supports it (`AudioEngine.cpp:444-463`); F3DAudio has the layouts. Optional.
  - Distance examples (RPC 868, a unit at the focus):

    | Zoom | Moho | Engine |
    |---|---|---|
    | 300 | −898 mB | −714..−788 mB |
    | 1000 | −2695 mB | −2382..−2512 mB |
    | 2000 | −4101 mB | −3502..−3730 mB |

    The engine is 1–6 dB louder when zoomed out.

  **Size: M.**

### 9. World-sound filtering (`CUserSoundManager::UpdateSoundRequests`)
- **(b)** Sim sounds are queued as `SAudioRequest`s (`CSimSoundManager.cpp:122-131`) and consumed on the user side each beat:
  - skipped entirely while world sounds are disabled (`:1327`);
  - `FilterSound` (`:2241-2272`):
    - LodCutoff against CameraDistance;
    - an LOS test on the focus army's recon grid (mask 0x01, or Fog 0x02 for seabed/sub layers; passes with no army or with fog of war off). `snd_CheckLOS` defaults true (`misc/RuntimeTuningGlobals.cpp:13`);
  - one per (bank, cue) per beat (`:1413-1428`);
  - entity loops:
    - started only for entities returned by `GetAllSoundEntitiesInFrustum`, and only when CameraDistance ≤ 200 (`:67,1542-1573`);
    - re-filtered each beat: distance-culled loops are destroyed, LOS-culled ones stopped with release (`:1459-1505`);
    - `Sound_*` stats (`:1320-1325`).
- **(c)** Entity, unit and weapon `PlaySound` call `mgr->play` directly at sim time (`lua/bindings/sim/entity.cpp:101`, `unit.cpp:120`, `weapon.cpp:107`). Ambient loops start on `SetAmbientSound` whatever the camera or fog (`entity.cpp:117`, `unit.cpp:147`).
- **(d)**
  - Add a per-frame audio filter on the app side: a focus-army visibility predicate (the engine has the recon grid), the per-beat dedupe, frustum gating and the CameraDistance ≤ 200 start rule for loops.
  - Loops become "wanted" state on the entity, started and stopped by the filter, as in Moho, rather than started by the sim binding.

  **Size: M–L.** This is the largest item. It fixes the fog leak and big-battle silence.

### 10. Entity ambient loops
- **(a)** 437 ambient-slot blueprint entries resolve to looping cues. 14 resolve to one-shots: 13 × `StartMoveSub`, plus `UEL0203_Move_Water_Lp`. `XSL0301_Capture_Loop` is missing from the retail data.
- **(b)** The cue plays as authored. A loop whose cue has stopped is destroyed (`CUserSoundManager.cpp:1841-1845`). Non-immediate stop (`:1194`).
- **(c)** `play_loop` forces infinite looping (`sound_manager.cpp:526-538,550`).
- **(d)** Drop `force_loop`. **Size: S.**

### 11. Duck
- **(a)** 1,744 sounds carry the Duck RPC, down to −10 dB.
- **(b)**
  - `PushDuck` starts a ramp. Duck = elapsed / DuckLength (0.5 s) going up, and 1 − elapsed / DuckLength coming down (`CUserSoundManager.cpp:1964-2008,2197-2231`).
  - Only `StopSound(h, true)`, `StopAllSounds` and `SetVolume` pop it (`:2066,2130-2140,2147-2152`). faf-re shows **no pop when a ducking voice ends by itself**, which is what happens to `UserSync.lua:42`'s discarded handles.
- **(c)** Duck is stepped to 1 at start and 0 at end (`moho_bindings.cpp:3733-3736`).
- **(d)**
  - Ramp over DuckLength both ways: Duck at 50/125/250 ms → −185/−470/−945 mB, and −1000 mB from 265 ms.
  - Keep the pop at natural end (a deliberate divergence, pending a check that faf-re isn't simply missing the call).
  - Reset on SetVolume.

  **Size: S.**

### 12. Pause
Covered in headline 4. The fix: `SoundManager::pause_category(name, bool)` over the category subtree:
- `ma_sound_stop` without seeking (keep the cursor), and resume with `ma_sound_start`;
- headless: freeze the instance's clock (shift `fire_at`, `ends`, `stop_started` and `started` by the paused time);
- fades and RPC time (AttackTime/ReleaseTime) freeze too.

`PauseVoice` affects the VO banks; with one registry that is the same call. **Size: S.**

### 13. VO banks, language and tutorial VO
- **(b)** `AudioSetLanguage(la)` (from `Localization.lua:43-46`, called with 'us' when no localised VO exists) creates engines for `/sounds/voice/<la>` and `/sounds/voice/<la>/tutorials` (`sim/Sim.cpp:26046-26056`).
  - `PlayVoice` plays on the VO engine (`CUserSoundManager.cpp:3003`).
  - `PlayTutorialVO` plays on the tutorial engine (`:2934`); retail Lua never calls it.
  - `CSndParams` resolves a bank to whichever engine owns it (`SND_FindEngine`).
- **(c)** Nothing loads `Voice/*`; the `AudioSetLanguage`/`HasLocalizedVO` stubs are listed in headline 1. `PlayTutorialVO` is **not bound**, although the M216a doc says it is.
- **(d)**
  - The registry takes several directories: `sounds/`, `sounds/Voice/<LANG>` (found case-insensitively on Linux) and `.../tutorials`.
  - `AudioSetLanguage` (re)loads the VO directory; `HasLocalizedVO` checks the directory exists.
  - Bind `PlayTutorialVO`.
  - Extend `--audio-data-test` to the 1,275 VO cues.
  - Note `Seraphim_Language.xwb` (no sound bank of its own) and the VO banks' cross-references.

  **Size: S.**

### 14. Music transitions
- **(a)** No XACT interactive music: no transitions, no cue variation tables. Transitions are retail Lua, `UserMusic.lua`:
  - battle: `StopSound(Music, true)` + `PlaySound(Battle)`, a hard cut;
  - peace: `StopSound(Music)` (release) → `WaitFor` → 3 s → `PlaySound(Base_Building)`;
  - cue replacement (`Main_Menu` → game music): the `Music` category, limit 1, ReplaceOldest, 200 ms crossfade.
- **(c)** The Lua flow works. Gaps: no track cycling within a cue (headline 3); a replacement uses the 6 s release (§5).
- **(d)** Covered by §2 and §5. **Size: none extra.**

### 15. Movies and ADX
- **(a)** 1,003 of the 1,004 movies have **no audio stream**. Their sound is XACT cues passed to `Movie(parent, file, sound, voice)` (`mohodata/lua/maui/movie.lua:30-60`): `FMV_BG` music and the voice from `X_FMV` or `X0n_VO`. Only `e3_demo_cut.sfd` carries ADX (header `80 00 01 1C 03 12 04 02`, 48 kHz stereo), and nothing references it.
- **(b)** `CMovie` plays through the Sofdec player (`movie/CMovie.cpp:384-421`), which would decode an embedded ADX track with its own runtime (`audio/SofdecAdxDecodeRuntime.cpp`) and output it outside XACT.
- **(c)** pl_mpeg with audio disabled (`src/video/video_decoder.cpp:48`). The engine plays the XACT side (M216a), but the movie voices are silent because of headline 1.
- **(d)** Retail needs only the Voice/US fix. ADX, for mods, FAF and e3_demo:
  - demux the MPEG-PS 0xC0 packets (pl_mpeg's demuxer, or a 60-line PS walker);
  - an ADX decoder: 18-byte frames = 2-byte scale + 32 × 4-bit samples; fixed 2-tap predictor from the header's high-pass cutoff (500 Hz);
  - an `ma_data_source` ring buffer started with the movie and paused with it.

  **Size: M. Defer.**

### 16. Streaming and hitches
- **(a)** 22 streaming banks. The largest waves: Music 34 MB (193 s), FMV_BG 43 MB, X_FMV 36.5 MB.
- **(c)** `wave_data` reads the whole wave and copies it into a WAV buffer synchronously on the main thread (`sound_manager.cpp:237-271`); the LRU holds 128 MB. With music cycling fixed, every track change is a ~34 MB read plus a copy.
- **(d)** A PCM file-range `ma_data_source` that reads on demand, for streaming banks, or async preload on prepare. **Size: S–M.**

### 17. Miscellaneous
- `StopAllSounds`:
  - Moho: destroys entity loops and script sounds, then stops `Global` **with release** (`CUserSoundManager.cpp:2083-2141`, `Stop(categoryId, 0)` at `:2118`).
  - Engine: immediate (`sound_manager.cpp:654-657`). Retail calls it on the score screen (`score.lua:220-221`).
  - Fix: S.
- `DisableWorldSounds`:
  - Moho gates new world *requests* (`:1327`); it doesn't mute.
  - The engine mutes the World subtree (`sound_manager.cpp:337`). Retail uses Disable on the score screen and Enable at `gamemain.lua:78`.
  - Fix: S, as part of §9.
- ReplaceLowestPriority (12 cues): the engine and FAudio replace the smallest priority value. XACT's priority orientation is unverified.
- Case: 1 retail reference resolves only case-insensitively (`Impacts/XSB2305_impact`). XACT cue lookup is case-sensitive, as far as I know.
- Clipping: Global is +6 dB, so sums can exceed 0 dBFS. That is true in XACT too; not a gap.

## What can be verified without listening

Numeric and structural checks. Each yields pass/fail in CI or a data test.

1. **Parser semantics from retail data** (data test):
   - for each effect-variation flag value, the unused parameter's range is the tool default: 0x40-only → volume bytes (151, 214) on 129/129; 0x80-only → pitch (−100, 100) on 100/100;
   - the variation word bit 0x40 is set on exactly `Music/Base_Building` and `Music/Battle`;
   - every RPC parameter is 0 and every point type 0.
2. **VO coverage.** `--audio-data-test` over `sounds/` and `Voice/US`: 3,171 cues resolve. `PlayVoice(Sound{Bank='XGG',Cue='Computer_Computer_MissileLaunch_01351'})` returns a handle.
3. **Mix-probe unit tests.** Add `SoundManager::probe(handle)` → per voice {wave id, cents, mB breakdown (sound, track, variation, each RPC, category chain, fade, duck), L/R gains}. Assert against golden values computed independently from the XGS and XSB (Python):
   - Angle 1437: 180° → −23.0; 135° → −346.0; 110° → −943.1; 90° → −2016.6 mB;
   - Distance 868: d = 296 → −898 mB;
   - Duck ramp values from §11;
   - release: Music 6.06 s, 1027 → 2.99 s, 1282 → 1.01 s;
   - category chain: `UnitsUEF` sound = sound mB + (601 + 200 + 1 − 202) mB;
   - pan: emitter at listener-right (azimuth 90°) → (0, 1); straight ahead → (0.5, 0.5); 45° → (0.25, 0.75). F3DAudio linear law; compare to a port of `ComputeEmitterChannelCoefficients`.
4. **Offline render.** miniaudio `noDevice` engine plus `ma_engine_read_pcm_frames`:
   - render a cue; assert channel RMS ratios (pan) and duration;
   - pitch by zero-crossing or FFT: `UI_Menu_Rollover` must show components at +800 and +1200 cents relative to track 0's wave (a direct test of the flag fix);
   - music: across N loops, wave ids differ between neighbours and cover all 5 or 4 waves;
   - pause: silent output while paused, the sample cursor unchanged.
5. **Headless timing and limits** (existing harness):
   - Ambient cues start at full gain (no 1 s ramp);
   - a Music replacement fades the victim out in 200 ms;
   - a one-shot in an ambient slot ends by itself;
   - the per-beat dedupe (20 same-cue plays in one tick → 1);
   - loops: none started for entities off-camera or when CameraDistance > 200; none for entities in the focus army's fog;
   - `Sound_ActiveEntityLoops` ≤ category limits in a 4-AI skirmish.
6. **Listener geometry.** A unit test on the listener point (focus + (0, zoom − 4, 0)) and on the distance to a focus-point emitter (= zoom − 4).
7. **Oracles (optional).**
   - FAudio (zlib) built as a test tool. It supports content 43 with the correct 3.0 flag mapping. With `SDL_AUDIODRIVER=dummy`, compare `FACTCue_GetProperties` (active wave, variation index) and the computed volumes and pitches per cue.
   - Retail FA under Wine with `/spewsound` logs the `SND: 1shot/Loop/LoopRPC/DestroyEntityLoop` sequence for a replay, to diff against engine logs. Only if Wine can run here.

## Open questions (need the binary, XACT docs, or ears)

1. Stop order for a cue with both a cue fade-out and a release RPC: Music, 200 ms vs 6.06 s.
2. Do category volumes and instance limits apply hierarchically in XACT 3.0? FAudio: not for limits, quirky for volume.
3. The axis convention of Moho's per-cue `Angle` (the faf-re helper has no address).
4. Does Moho really leave Duck up after a ducking voice ends by itself?
5. XACT track-variation state per cue instance (FAudio) or per sound bank (engine)?

## Prioritised plan (smallest high-impact first)

| # | Item | Size | Impact |
|---|---|---|---|
| 1 | Swap the effect-variation flags (0x40 pitch, 0x80 volume); parse the other 3.0 bits; fix the fixture and test; add the retail-signature data test | S (≤½ d) | 229 events: UI pitched sounds, air pitch spread, land volume spread |
| 2 | Load `sounds/Voice/<lang>` and `tutorials`; real `AudioSetLanguage`/`HasLocalizedVO`; bind `PlayTutorialVO`; extend the audio data test | S | All EVA, campaign, briefing and movie VO |
| 3 | New-variation-on-loop plus exclusion-based no-repeat | S | Music cycles its 5/4 tracks |
| 4 | `PauseSound`/`PauseVoice` category pause (subtree; clocks frozen) | S | Game pause, briefing and transmission pause |
| 5 | Drop `force_loop` for ambient slots | S | 14 blueprints' one-shots stop repeating |
| 6 | Fade semantics: fade-in only on replacement; replacement fades out over the limiter's fadeOut; no category fade on stop; limits at start | S | Ambient 1 s fade-in; 6 s music crossfade |
| 7 | Duck ramp over DuckLength; reset on SetVolume; `/nomusic` | S | Duck pumping |
| 8 | Per-cue Angle RPC (after checking the axes) | S | 1,728 world sounds: −4.4 dB constant → correct |
| 9 | Moho listener point and basis; X3DAudio stereo matrix per voice (no miniaudio spatialiser); per-frame interpolated loop positions | M (1–2 d) | Panning restored; world mix −6..0 dB at centre; Distance 1–6 dB |
| 10 | `StopAllSounds` with release; `DisableWorldSounds` as a request gate | S | Score-screen cut-off |
| 11 | Streaming data source for streaming banks | S–M | 34–43 MB hitches at track and VO starts |
| 12 | World-sound filter: focus-army LOS, per-beat one-shot dedupe, frustum-gated loops with the CameraDistance ≤ 200 start rule, LodCutoff against CameraDistance | M–L (2–3 d) | Fog leak; big-battle loop starvation |
| 13 | ADX in Sofdec (demux + decoder + ma data source) | M, **defer** | Only the unreferenced `e3_demo_cut.sfd` in retail |
| 14 | `RPCSound` shared RPC loops (Rumble variables) | M, **defer** | Dead in retail data |

Items 1–8 total about 3–4 days and fix the most audible differences. Items 9 and 12 are the structural fidelity work. Every item can be gated by the non-listening checks above. The listening pass then only has to confirm the open questions.
