# Multiplayer Player-Drop / Timeout Handling — Design

Date: 2026-07-04
Status: approved, ready for implementation plan
Depends on: LAN lobby lifecycle (PRs #12–#16) — `LockstepSession`, `CommandScheduler`,
`mp_net_state`, the `--lan-host`/`--lan-join` harness, and the existing defeat /
share-rule dispose path in `SimState`.

## Goal

Stop a networked match from hanging forever in the "waiting for players" stall when
a peer disconnects or freezes. The surviving client detects the drop by timeout,
un-stalls, defeats the dropped player's army (disposing its units per the game's
share rule), and lets victory resolve normally — in 1v1 the survivor wins and
reaches the score screen.

### In scope
- Confirmation-timeout drop detection in `LockstepSession` (~3s / 30 command frames,
  configurable).
- `CommandScheduler::remove_source` so the survivor stops waiting on the dropped peer.
- `SimState::defeat_army(army)` reusing the existing defeat + `dispose_defeated_army`.
- Game-loop + harness wiring: on drop, defeat the mapped army.
- Headless two-process verification (a client that exits mid-match).

### Out of scope (documented follow-ups)
AI takeover of the dropped army, reconnect/rejoin, a "player dropped" UI toast (a log
line for now), >2-player drop consensus, distinguishing a clean disconnect from a
freeze (both are handled by the same timeout).

## Architecture

```
peer stops confirming (disconnect / freeze)
        │  behind = next_frame_ - peer_confirmed[src] grows ~1 per round
        ▼
LockstepSession: behind > drop_timeout_frames_  → drop src
        │  command_scheduler().remove_source(src)   (survivor un-stalls)
        │  newly_dropped_.push_back(src)
        ▼
game loop: for src in session.take_dropped(): sim.defeat_army(src)
        │  set_state(Defeat) + dispose_defeated_army(army)  (share rule)
        ▼
player_result() resolves → GAME → SCORE (survivor wins)
```

## Component 1 — CommandScheduler::remove_source

`command_scheduler.hpp`. Today `ready_to_run(tick)` returns false until *every*
registered source has confirmed `>= tick`. A dropped source never confirms, so the
sim stalls forever. Add:

```cpp
void remove_source(u32 source) {
    auto it = std::lower_bound(sources_.begin(), sources_.end(), source);
    if (it != sources_.end() && *it == source) sources_.erase(it);
    confirmed_frame_.erase(source);
}
```

After removal, `ready_to_run` no longer waits on that source. (`sources_` is the
sorted-unique participant vector; `confirmed_frame_` is the per-source map.)

## Component 2 — LockstepSession drop detection

`lockstep_session.{hpp,cpp}`. The session already ingests each peer frame's
`(source, frame)` in `receive_and_advance` (calling `confirm_frame`). Track the
peer's last-confirmed frame locally and, each round, measure how far behind it is.

State added:
- `u32 drop_timeout_frames_ = 30;`
- `std::unordered_map<u32, u32> peer_confirmed_;` (source → last frame it confirmed)
- `std::vector<u32> dropped_;` (sources already declared dropped — never re-drop)
- `std::vector<u32> newly_dropped_;` (drained by the game loop)

Logic:
- In `receive_and_advance`, when a peer frame arrives, set
  `peer_confirmed_[source] = max(peer_confirmed_[source], frame)`.
- At the **end** of `receive_and_advance` (after the advance-while loop, so
  `peer_confirmed_` is current and `next_frame_` reflects the latest `send_frame`),
  for every source that (a) is not `local_source_`, (b) is not already in `dropped_`,
  and (c) **has confirmed at least one frame** (`peer_confirmed_.count(src) > 0` — a
  source is only armed for timeout after its first confirmation, so a slow first
  frame at session start can't false-drop it; the lobby handshake already proved both
  peers live at launch): compute `behind = next_frame_ - peer_confirmed_[src]`. If
  `drop_timeout_frames_ > 0 && behind > drop_timeout_frames_`, mark dropped:
  `dropped_.push_back(src); newly_dropped_.push_back(src);
  sim_.command_scheduler().remove_source(src);` and log it.
- `next_frame_` keeps advancing each `send_frame()` even while stalled, so a
  once-live peer that goes silent has `behind` grow ~1 per round; after
  `drop_timeout_frames_` rounds it trips.

API added:
- `void set_drop_timeout(u32 frames)` (0 disables detection).
- `std::vector<u32> take_dropped()` — moves out `newly_dropped_` (empty after).
- `bool has_dropped(u32 src) const`.

Determinism note: drop handling is 1v1 with a single survivor, so there is no
cross-client agreement to maintain — the lone survivor decides locally. (>2-player
consensus is out of scope.)

## Component 3 — SimState::defeat_army

`sim_state.{hpp,cpp}`. Extract the existing inline defeat (currently in the
elimination pass, ~sim_state.cpp:1163-1173) into a public, idempotent method:

```cpp
void SimState::defeat_army(i32 army) {
    if (army < 0 || static_cast<size_t>(army) >= armies_.size()) return;
    auto& b = armies_[static_cast<size_t>(army)];
    if (b->is_defeated()) return;                 // idempotent
    b->set_state(BrainState::Defeat);
    dispose_defeated_army(army);
    spdlog::info("Army {} ({}) defeated (player drop)", army, b->name());
}
```

The elimination pass can call it too (optional tidy), but the minimum is exposing it
for the drop handler. `player_result()` / `surviving_team_count()` already treat a
`Defeat`-state army as out.

## Component 4 — Game-loop + harness wiring

After each `session->receive_and_advance()` (the in-game MP tick branch in
`main.cpp`, and both `run_mp_lan_test` / `run_lan_lobby_test` harness loops):

```cpp
for (u32 src : session->take_dropped()) {
    spdlog::warn("[mp] peer source {} dropped — defeating its army", src);
    if (sim_state) sim_state->defeat_army(static_cast<i32>(src));
}
```

Source index maps to army index for 1v1 (host source 0 → army 0, client source 1 →
army 1). In the minimal-sim harnesses there are no army brains, so `defeat_army` is a
guarded no-op there; the harness instead asserts the drop was *detected* (see below).

## Testing

- **Unit — `test_command_scheduler.cpp`**: after `add_source(0)/add_source(1)` and
  `confirm_frame(0, 5)`, `ready_to_run(5)` is false; `remove_source(1)` makes it true.
- **Unit — `test_lockstep.cpp`**: two `LockstepSession`s over a `LoopbackHub`; drive
  ~N rounds, then stop pumping one side; the other reports `take_dropped()` containing
  the silent source after ~`drop_timeout_frames_` rounds, and then advances past the
  stall tick.
- **Two-process (headless)** — a `--mp-drop-at <frame>` flag on the LAN harness makes
  the client `std::exit(0)` at that frame. The host must: detect the drop within the
  timeout, continue to its full `--mp-frames` count (no stall), and print
  `dropped=1`. Run: host `--lan-host --mp-frames 60`, client
  `--lan-join 127.0.0.1 --mp-drop-at 20`. Expect host `LAN_RESULT ... dropped=1
  stalled=0 tick=60`.
- **Regression**: full `osc_tests` green; existing `--lan-host`/`--lan-join` (no drop)
  still syncs with `dropped=0`; SP `--full-smoke-test` clean (`remove_source` /
  `defeat_army` are unreachable in SP).

## Error handling

- Only remote sources are droppable (`local_source_` excluded).
- Already-dropped sources are never re-dropped (`dropped_` set).
- `defeat_army` bounds-checks the army index and is idempotent.
- `set_drop_timeout(0)` disables detection (e.g. for a deterministic replay).

## Success criteria

- New unit + integration tests green; full suite green; SP full-smoke clean.
- `--lan-host` + `--lan-join --mp-drop-at N`: the host detects the drop, defeats the
  peer's army (in a real game), and finishes without stalling; harness reports
  `dropped=1 stalled=0`.
- A normal (no-drop) LAN match is unchanged (`dropped=0`, still syncs).
