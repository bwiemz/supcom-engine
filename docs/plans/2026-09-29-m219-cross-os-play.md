# M219: cross-platform play

## Why

The roadmap asks for Windows ↔ Linux play, with a CI job that runs it.
Phase D made the sim deterministic across the two (M199's cross-OS replay
check: a Windows build under Wine and a Linux build play a recorded game to
the same checksum at every tick). What no check covered was the two playing
**together**: the lobby's connections, the lockstep's messages and pace,
and each side's sim agreeing live, tick for tick, with a Windows peer on
Winsock and MSVC's C runtime and a Linux peer on POSIX sockets and glibc.

## The check

The data-free lockstep pairs (`run_pair.py`, since M218g over a lobby's
connections: host, join, launch, then minimal sims in lockstep) take their
joiner from another build (`--joiner-exe`); off Windows, a `.exe` runs
under Wine. `tests/integration/cross_os_pairs.py` plays each pair twice, a
Linux build hosting a Windows one and the other way round:

- in sync: both reach the same checksum, and each checks the other's every
  tick (a divergence is a desync, which fails the pair);
- a divergence (a local-only order on the host) caught by both;
- a joiner gone without a goodbye, dropped while the host plays on;
- a joiner at half speed setting the pace, no one dropped.

The CI job `cross-os-play` needs both builds: the Linux gcc job uploads
its integration runner (stripped: 400 MB with its debug info), the
Windows job its Release build (already uploaded for M199's replay check;
Release since Wine has no debug C runtime). It installs Wine and the
Vulkan loader, and runs the driver.

## Not covered

The pairs' sims are minimal (a few units moving on a flat world, no game
data): what they prove is the network and the core sim's arithmetic. A
full game's cross-OS agreement is M199's replay check, run locally (CI has
no game data): `tools/cross_os_replay.py`.
