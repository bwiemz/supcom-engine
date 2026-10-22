# Simulation thread ownership

The portable WOPC thread design keeps one simulation job in flight.
`SimThread` owns the thread, captures job exceptions, and rethrows them
when the main thread collects the job. Window teardown joins it before
members captured by the job are destroyed.

A settled frame collects the job, publishes its captured world and queued
Sync packets, delivers UI notifications, handles input and prepares the
render. Only after render preparation finishes does it launch another job.
The job runs during frame pacing and subsequent busy frames. A busy frame
updates the camera and interpolates the published world while reusing UI,
world-view layout, placement previews and custom world geometry.

Rendering owns a terrain copy; deformations publish only on settled frames.
Busy draws do not execute Lua. Script-dependent asset cache misses wait
for a settled frame and remain retryable. Resize and camera-tracking Lua
notifications also wait until collection. Sim-to-UI Sync is captured after
every beat, including paused beats; the main thread alone delivers it.
UI focus requests transfer into sim-owned storage before a job starts.

Each job stages events per beat and captures the final world once. Worker
chunks write disjoint world records and join before publication. Visibility
retains main's serial callback order. A drawing clock follows published
sim time with a buffered delay, including when a job produces several ticks.

`--no-sim-thread`, catch-up and ordinary profiling use serial simulation.
`--profile-frames` keeps threading enabled and profiles frame work. Thread
ownership checks at Lua API boundaries catch accidental sim-state access
from the drawing thread in test and benchmark runs.

The source design and measurements are in WOPC merge requests !60 and !69.
Those measurements describe the original fork, not this port. Validation
must compare checksums with threading/workers enabled and disabled, exercise
real UI rendering and network resume, and check continued whole-game replays.
