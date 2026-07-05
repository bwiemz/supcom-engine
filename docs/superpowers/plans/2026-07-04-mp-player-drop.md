# Multiplayer Player-Drop / Timeout Handling Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A networked match no longer hangs when a peer disconnects — the survivor times out the drop, un-stalls, defeats the dropped army, and finishes.

**Architecture:** `LockstepSession` measures how far behind each peer's frame confirmations are; past a ~30-frame threshold it declares the source dropped, removes it from the `CommandScheduler` participant set (un-stalling the sim), and reports it. The game loop defeats the mapped army via the existing share-rule dispose path.

**Tech Stack:** C++17, CMake (VS2022), Catch2.

## Global Constraints

- Build: `cmake --build build --config Debug` (cmake at `C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`, `VCPKG_ROOT=C:\vcpkg`, neither on PATH).
- Tests: `build/tests/Debug/osc_tests.exe [tag]`.
- Single-player must be unaffected: `remove_source`/`defeat_army` are only reachable in multiplayer; `defeat_army` is a guarded, idempotent helper.
- Determinism: drop handling is 1v1 (single survivor), so no cross-client agreement is needed.
- Commit after each task; message ends `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`.

---

### Task 1: CommandScheduler::remove_source

**Files:**
- Modify: `src/sim/command_scheduler.hpp` (add `remove_source`)
- Test: `tests/test_command_scheduler.cpp` (add a case)

**Interfaces:**
- Produces: `void CommandScheduler::remove_source(u32 source)`.

- [ ] **Step 1: Add the failing test to `tests/test_command_scheduler.cpp`** (append a TEST_CASE; it uses the existing `osc::sim::CommandScheduler`):
```cpp
TEST_CASE("remove_source stops ready_to_run waiting on a dropped peer",
          "[scheduler]") {
    osc::sim::CommandScheduler s;
    s.set_lockstep(true);
    s.add_source(0);
    s.add_source(1);
    s.confirm_frame(0, 5); // only source 0 confirmed
    REQUIRE_FALSE(s.ready_to_run(5)); // still waiting on source 1
    s.remove_source(1);
    REQUIRE(s.ready_to_run(5)); // source 1 no longer a participant
}
```

- [ ] **Step 2: Run to confirm it fails** — `osc_tests.exe "[scheduler]"` → compile error (`remove_source` undefined).

- [ ] **Step 3: Implement `remove_source`** in `command_scheduler.hpp`, next to `add_source`:
```cpp
    /// Stop treating `source` as a participant (e.g. a dropped peer), so
    /// `ready_to_run` no longer waits on its confirmations.
    void remove_source(u32 source) {
        auto it = std::lower_bound(sources_.begin(), sources_.end(), source);
        if (it != sources_.end() && *it == source) sources_.erase(it);
        confirmed_frame_.erase(source);
    }
```
(`<algorithm>` is already included in this header for `std::lower_bound`.)

- [ ] **Step 4: Build + test** — `osc_tests.exe "[scheduler]"` PASS.
- [ ] **Step 5: Commit** — `feat: CommandScheduler::remove_source to drop a peer from the lockstep gate`.

---

### Task 2: LockstepSession drop detection

**Files:**
- Modify: `src/sim/lockstep_session.hpp` (state + API), `src/sim/lockstep_session.cpp` (logic)
- Test: `tests/test_lockstep.cpp` (add a case)

**Interfaces:**
- Consumes: Task 1 `CommandScheduler::remove_source`.
- Produces: `void LockstepSession::set_drop_timeout(u32 frames)`, `std::vector<u32> take_dropped()`, `bool has_dropped(u32) const`. Default `drop_timeout_frames_ = 30`.

- [ ] **Step 1: Add state + API to `lockstep_session.hpp`** — in the `public:` section add:
```cpp
    // Declare a peer dropped once it is more than `frames` command frames behind
    // in confirmations (default 30 ≈ 3s at 10 Hz). 0 disables detection.
    void set_drop_timeout(u32 frames) { drop_timeout_frames_ = frames; }
    // Sources newly declared dropped since the last call (drained).
    std::vector<u32> take_dropped();
    bool has_dropped(u32 src) const;
```
and in `private:` add:
```cpp
    u32 drop_timeout_frames_ = 30;
    std::unordered_map<u32, u32> peer_confirmed_; // source -> last confirmed frame
    std::vector<u32> dropped_;                    // already declared dropped
    std::vector<u32> newly_dropped_;              // drained by take_dropped()
```
(`<unordered_map>` and `<vector>` are already included.)

- [ ] **Step 2: Implement in `lockstep_session.cpp`** — (a) in `receive_and_advance`, inside the per-frame ingest loop, right after `sim_.command_scheduler().confirm_frame(source, frame);`, record the peer's frame:
```cpp
        u32& pc = peer_confirmed_[source];
        if (frame > pc) pc = frame;
```
(b) at the very end of `receive_and_advance` (after the `while (... ready_to_run ...)` advance loop), add the drop check:
```cpp
    if (drop_timeout_frames_ > 0) {
        for (const auto& [src, confirmed] : peer_confirmed_) {
            if (src == local_source_) continue;
            if (std::find(dropped_.begin(), dropped_.end(), src) != dropped_.end())
                continue;
            u32 behind = next_frame_ > confirmed ? next_frame_ - confirmed : 0;
            if (behind > drop_timeout_frames_) {
                dropped_.push_back(src);
                newly_dropped_.push_back(src);
                sim_.command_scheduler().remove_source(src);
                spdlog::warn("[lockstep] peer source {} timed out ({} frames "
                             "behind) — dropped", src, behind);
            }
        }
    }
```
(c) add the two accessors at the bottom of the file (inside `namespace osc::sim`):
```cpp
std::vector<u32> LockstepSession::take_dropped() {
    std::vector<u32> out;
    out.swap(newly_dropped_);
    return out;
}
bool LockstepSession::has_dropped(u32 src) const {
    return std::find(dropped_.begin(), dropped_.end(), src) != dropped_.end();
}
```
(d) add `#include <algorithm>` and `#include <spdlog/spdlog.h>` to `lockstep_session.cpp` if not already present (grep first; `<cstring>` is there — add the two if missing).

- [ ] **Step 3: Add the failing/behavior test to `tests/test_lockstep.cpp`** — a two-session loopback where one side goes silent:
```cpp
TEST_CASE("LockstepSession times out a silent peer", "[lockstep][drop]") {
    osc::sim::LoopbackHub hub;
    LuaGuard ga, gb; // existing helper in this file
    osc::sim::SimState a(ga.L, nullptr), b(gb.L, nullptr);
    osc::sim::LoopbackTransport ta(hub, hub.add_endpoint());
    osc::sim::LoopbackTransport tb(hub, hub.add_endpoint());
    osc::sim::LockstepSession sa(a, ta, 0, {0, 1});
    osc::sim::LockstepSession sb(b, tb, 1, {0, 1});
    sa.set_drop_timeout(5);
    // Exchange a few healthy rounds so peer_confirmed_ is armed.
    for (int r = 0; r < 3; ++r) {
        sa.send_frame(); sb.send_frame();
        sa.receive_and_advance(); sb.receive_and_advance();
    }
    REQUIRE(sa.take_dropped().empty());
    // B goes silent; only A keeps sending. A must drop source 1 within ~5 rounds.
    bool dropped = false;
    for (int r = 0; r < 30 && !dropped; ++r) {
        sa.send_frame();
        sa.receive_and_advance();
        auto d = sa.take_dropped();
        if (std::find(d.begin(), d.end(), 1u) != d.end()) dropped = true;
    }
    REQUIRE(dropped);
    REQUIRE(sa.has_dropped(1));
    // After the drop, A is no longer stalled — it can advance past the gate.
    osc::u32 before = a.tick_count();
    sa.send_frame(); sa.receive_and_advance();
    REQUIRE(a.tick_count() > before);
}
```
(If `test_lockstep.cpp` lacks a `LuaGuard`/`#include <algorithm>`, add them — check the top of the file first.)

- [ ] **Step 4: Build + test** — `osc_tests.exe "[lockstep]"` PASS; full `osc_tests.exe` PASS (no-drop lockstep tests unaffected — default timeout 30 won't trip in short tests).
- [ ] **Step 5: Commit** — `feat: LockstepSession confirmation-timeout drop detection`.

---

### Task 3: SimState::defeat_army + game-loop/harness wiring + two-process verification

**Files:**
- Modify: `src/sim/sim_state.hpp` (declare `defeat_army`), `src/sim/sim_state.cpp` (implement; optionally reuse in the elimination pass)
- Modify: `src/main.cpp` (drain `take_dropped()` after `receive_and_advance` in the in-game MP tick branch, `run_mp_lan_test`, and `run_lan_lobby_test`; add `--mp-drop-at <frame>` to the LAN harness client)
- Modify: `docs/current-state.md`
- Test: two-process run

**Interfaces:**
- Consumes: Task 2 `LockstepSession::take_dropped()`; existing `SimState::dispose_defeated_army`, `ArmyBrain::set_state`/`is_defeated`, `BrainState::Defeat`.
- Produces: `void SimState::defeat_army(i32 army)`.

- [ ] **Step 1: Declare + implement `defeat_army`** — in `sim_state.hpp` (public, near `player_result`): `void defeat_army(i32 army);`. In `sim_state.cpp` (near `dispose_defeated_army`):
```cpp
void SimState::defeat_army(i32 army) {
    if (army < 0 || static_cast<size_t>(army) >= armies_.size()) return;
    auto& b = armies_[static_cast<size_t>(army)];
    if (!b || b->is_defeated()) return; // idempotent
    b->set_state(BrainState::Defeat);
    dispose_defeated_army(army);
    spdlog::info("Army {} ({}) defeated (player drop)", army, b->name());
}
```

- [ ] **Step 2: Drain drops in the in-game MP tick branch** — in `main.cpp`, in the `if (osc::lua::mp_net_state().active())` tick branch, right after `session->receive_and_advance();`, add:
```cpp
                            for (osc::u32 src : session->take_dropped()) {
                                spdlog::warn("[mp] peer {} dropped — defeating army",
                                             src);
                                if (sim_state)
                                    sim_state->defeat_army(static_cast<osc::i32>(src));
                            }
```

- [ ] **Step 3: Drain drops in both harnesses + add `--mp-drop-at`** — in `run_mp_lan_test` and `run_lan_lobby_test`, after `session->receive_and_advance();` inside the pump loop, add:
```cpp
            for (u32 src : session->take_dropped())
                spdlog::warn("[mp] peer {} dropped", src);
```
and add a drop counter for the result line: before the round loop, `u32 dropped_count = 0;`; accumulate `dropped_count += (u32)session->take_dropped().size();` — but since take_dropped drains, instead track it once: replace the drain above with
```cpp
            { auto d = session->take_dropped(); dropped_count += (u32)d.size();
              for (u32 s : d) spdlog::warn("[mp] peer {} dropped", s); }
```
Then in the `LAN_RESULT`/`MP_RESULT` printf add ` dropped=%d` with `(int)(dropped_count>0)`.
For the client exit: parse `--mp-drop-at` and, in the client's round loop, `if (!is_host && drop_at > 0 && round == drop_at) { std::fflush(stdout); std::exit(0); }`. Thread `u32 drop_at` into `run_lan_lobby_test`/`run_mp_lan_test` from the CLI dispatch (`parse_string_arg("--mp-drop-at","0")`).

- [ ] **Step 4: Build** — `cmake --build build --config Debug` → EXIT 0.

- [ ] **Step 5: Two-process drop verification** — terminal A: `opensupcom --lan-host --mp-port 47960 --mp-frames 60`; terminal B: `opensupcom --lan-join 127.0.0.1 --mp-port 47960 --mp-frames 60 --mp-drop-at 20`. Expected: B exits at ~round 20; A logs `peer 1 dropped`, continues to `tick=60` without stalling, and prints `LAN_RESULT ... dropped=1 stalled=0`.

- [ ] **Step 6: Regression** — `osc_tests.exe` all pass; a normal `--lan-host`/`--lan-join` (no `--mp-drop-at`) still syncs with `dropped=0`; `--full-smoke-test` clean.

- [ ] **Step 7: Docs + commit** — note drop handling in `docs/current-state.md`; commit `feat: defeat dropped peers on lockstep timeout + --mp-drop-at verification`.

---

## Self-review notes
- Spec coverage: remove_source (T1), LockstepSession detection (T2), defeat_army + wiring + verification (T3). All spec components mapped.
- Types consistent: `remove_source(u32)`, `take_dropped()->vector<u32>`, `has_dropped(u32)`, `set_drop_timeout(u32)`, `defeat_army(i32)`, `drop_timeout_frames_=30`.
- The drop check iterates `peer_confirmed_` (only sources that confirmed ≥1 frame), so it arms only after first contact — no false drop at session start.
- T3 Step 3's `dropped_count` bookkeeping is fiddly; when implementing, keep a single `session->take_dropped()` drain per loop iteration and accumulate its size, to avoid draining twice (take_dropped empties the list).
