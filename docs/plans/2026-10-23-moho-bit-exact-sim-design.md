# Moho's simulation bit for bit

Design, 2026-10-23. Up to now the engine has matched Moho's movement closely: the spline, the
steering and the pushes, with Moho's rules and their order. Close is not enough for a unit to do in
OpenSupCom what it does in Forged Alliance. In a crowd, a turn that goes the other way decides on the
fifth significant digit of a float. One example: in the cone probe's `cross90a` scene, the unit passes
its sidestep point with the point dead behind it. Then `x87`'s rounding decides which way it turns
round. This note plans a sim that computes what Moho's computes, bit for bit, on every platform the
engine builds for.

Path prefixes:

- **FR:** faf-re (Draiget/faf-re, the decompiled FAF build of Moho), `src/sdk/`
- **EN:** this repository's `src/`

## 1. Why it can be done

faf-re has done it for Windows. Its rebuilt engine plays back 76 of 76 replays from the FAF vault
checksum-identical to the retail `ForgedAlliance.exe` on every beat. A live three-player game with the
retail exe, the rebuilt x86 build and the rebuilt x64 build ran 21 minutes without a desync
(FR `docs/RETAIL_REPLAY_PARITY.md`, snapshot 2026-10-20). Its x64 build does its arithmetic on SSE,
not x87. Bit-exactness therefore does not need x87 arithmetic. It needs:

1. **Float arithmetic in Moho's order.** Single precision throughout, no fused multiply-add, and the
   same order of operations. Moho sums a dot product y, z, then x in places. The engine's build
   already forbids contraction (`/fp:precise`, `-ffp-contract=off`).
2. **The places where Moho kept a value on the x87 stack.** About fifteen inlined shapes keep an
   intermediate result in extended precision or exponent range before one float store (FR
   `legacy/math/X87Math.h`):
   - the scale in a vector's length clamp (`VecLimitLengthTo`, the spline's planar scaling);
   - an oriented box's radius on an axis (the box-vs-box separating-axis test);
   - a Mersenne Twister word times its factor (`CRandomStream::FRand`);
   - terrain's bilinear height;
   - a few half-sums and products.

   faf-re computes these in double or in software, so x86 and x64 agree. The same works on any
   platform.
3. **Moho's transcendentals.**
   - `sinf`, `cosf`, `tanf`, `atanf` and `atan2f` were the x87 instructions (`fsin`, `fcos`, `fptan`,
     `fpatan`), with intrinsics on.
   - `exp`, `log` and `log10` were the `/Oi` intrinsic sequences, with their multiply and add steps
     rounded under 24-bit precision control.
   - `asin`, `acos` and `pow` were the MSVC8 CRT's x87 bodies at that precision.

   None of these is correctly rounded, and none matches fdlibm (EN `core/dmath`) in every bit.
4. **Lua numbers as floats.** Moho's LuaPlus keeps a number in a float. EN's Lua 5.0 uses double, so
   every script computation in the sim rounds differently.
5. **Each system ported exactly.** That includes the random stream's call order, order validation,
   float-to-integer conversion (`fistp` rounds to nearest), unsigned cell coordinates, and so on. The
   causes of faf-re's past divergences are a checklist (FR `docs/RETAIL_REPLAY_PARITY.md`, "Where
   divergences came from").

## 2. Every platform

The engine must build and play the same game on Windows, Linux and macOS, including ARM (Apple M1
and later). Its code therefore never uses an x87 instruction. Instead it computes, in portable C++,
the floats Moho's x87 instructions produce:

- **One-argument functions** (`sinf`, `cosf`, `tanf`, `atanf`, `expf`, `logf`) are checked
  exhaustively. All 2^32 float inputs go through the real x87 instruction on an x86 machine and
  through EN's software version, and the two must agree bit for bit. A test keeps this on an x86 CI
  runner; ARM builds run the same software.
- **`atan2f`** has 2^64 inputs, too many to check them all. It is checked on a sample, plus inputs
  chosen at float rounding boundaries. FAF players on Intel and AMD processors play together without
  desyncs, which is evidence the x87 results agree across vendors on the inputs games produce.
- **What differs from today:**
  - `fsin` reduces a large argument with a 66-bit approximation of pi;
  - the CRT-era sequences round at each step.

  Both are reproducible in software. For a small argument, the extended result rounded to float
  almost always equals the correctly rounded float, which is also what fdlibm's double rounded to
  float gives.

Software trigonometry costs more than the hardware's; it is measured before it lands.

## 3. The plan

Each step ends on a check against retail, and only a step whose check passes is built on.

| Step | What | Check |
|---|---|---|
| 1 | Exact probes: the drive grid and cone probes print positions with 9 significant digits, enough to give a float back bit for bit; `drive_diff.py --trace` counts the legs that match bit for bit and where the others first differ. | A baseline of legs bit for bit, recorded here. |
| 2 | The sim's float math: `osc::dmath`'s float functions compute Moho's x87 results in software, plus the extended-precision shapes as EN helpers. Re-expressed from faf-re's descriptions, not copied. | Exhaustive and sampled tests against x87 on x86; the checksum test re-pinned; no data mode worse. |
| 3 | Lua numbers as floats. Not started: its own design note first. It touches every binding and FAF's data. | — |
| 4 | Movement bit for bit: the spline, the steering, the pushes and the navigator, against the exact probes until no leg differs. The open questions of the spline port (its section 4: tick 95, the coast after `SetWaypoints(0)`; `cross90a`'s turn) are settled here. | Drive grid and cone: every leg bit for bit; the crowd probe to its end. |
| 5 | The other systems, one at a time (weapons, projectiles, economy, ...), towards FAF replays playing back with their recorded checksums. | Per system, a probe bit for bit. |

Where `/synclog` and probes can't show a value (a steering prediction, a spline node), faf-re's code
answers it. If that is not enough, faf-re is built locally as an observation tool. Its build dumps the
whole sim state every beat, as FR `docs/RETAIL_REPLAY_PARITY.md` describes.

## 4. Provenance

faf-re has no licence file, and this repository's rules forbid copying decompiled code, even
translated (`docs/provenance-and-compatibility.md`). faf-re is read for Moho's rules: which value
stays on the x87 stack, the order of a sum, what an instruction returns. Each rule is written anew in
EN, cited by faf-re file and retail address, and tested against retail or x87 hardware.

## 5. Baseline (step 1)

The first exact runs (retail FA 3599 against the engine at b365a575, `--moho-pathing`) compare
floats, not text. The two C runtimes break a printing tie apart (640.3515625 prints `640.351563` in
retail and `640.351562` in the engine), so `drive_diff.py` reads each number back as a float before
comparing.

**Drive grid** (85 ground units, 337 legs):

- **Bit for bit:** 192 legs; all four legs of 27 units.
- **The first leg that differs:** leg 1 for 41 units, leg 2 for 7, leg 3 for 5, leg 4 for 5. The
  median tick of that difference is 9.
- **Within 0.01:** 258 legs.

The most common first difference is not a rounding. Two cases:

- `uel0101` and `uel0105` take their first step at tick 4 the mirror way of retail's, (-0.0186, 0.004)
  against (0.0186, -0.004), then stand a tick.
- `ual0101` starts a tick early.

**Cone** (8 scenes, 150 ticks each):

- **Bit for bit throughout:** 4 scenes (`overtake2`, `cross90b`, `headon1`, `headon2`).
- **First difference by a float's last bit:** `cross90a` at tick 80, `cross60` at 72 and `cross120`
  at 80, each in unit b's z as the two units meet (in `cross90a`, two ticks after they stop pushing
  each other).
- **A tick early:** `overtake1`'s engineer starts a tick early (tick 4).

## 6. Movement (step 4) so far

Retail FA 3599 (Steam) against the engine on `feat/moho-entity-ids`, rebased on c9055933,
`--moho-pathing`:

- **Drive grid:** 306 of 337 legs bit for bit (192 at the baseline), 330 within 0.01. Of the seven
  units of the second batch that started a tick early, one still does. Its start follows which trees
  fell before it, so it waits on the precision of meeting props.
- **Crowd probe:** bit for bit to its end (closest pair 0.899).
- **`url0001`:** off only in a float's last bits, step 2's work.
- **`crowd-test`'s scene:** retail's closest pair is 0.936. The engine's is 0.873 in the game (it
  parts from retail at tick 94) and 0.18 under `osc_integration`'s runner, so `data.moho.crowd-test`
  is a `retail-gap` until that difference is found.

The rules this round matched, from faf-re:

- **Path budget:** an army's path work is 1000 a tick, retail's (FAF's is 2500): expansions plus 10
  per cluster rebuilt.
- **Arrival:** an arriving unit stops its steering before it goes idle (`CAiNavigatorLand::Execute`).
- **Meetings:**
  - candidates are gathered as `CollectCollisionCandidates` and `func_IsSourceUnit`;
  - boxes met edge to edge make a contact of no depth;
  - the spline brakes for a unit standing where it would go (`Generate`'s `UnitIsBlockedAt`).
- **Props:** one worth reclaiming occupies its ground when made, whatever its caps (a tree has none,
  yet it dirties the path clusters under it), and releases it where it stands when destroyed. A unit
  meets props when its motion meets units (`MotionTick`: speed squared over 1e-6 or a surface
  collision, not while pushed), on its id's tick mod 5.
- **The spline's start:** the velocity it starts from is the motion's for a unit not in the air
  (`Unit::GetVelocity`); braking travel is velocity times `1 / speed`.
- **Through a waypoint:** at top speed when the waypoint's cell is in the goal and the goal is driven
  through, or when the path has ended and no path is awaited.
- **Pushed units:** a pushed unit settles once it is below 1% of its top speed
  (`CAiSteeringImpl::OnTick`).
- **Destination:** the steering's destination is the waypoint even when the unit stands at it
  (`DriveToNextWaypoint`).

Found on the way, and not Moho: an army with no units gets a commander (`session_manager.cpp`), and
without a lobby the victory condition defaults to demoralization, where retail's `/map` sets none.
