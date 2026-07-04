# Windowed LAN Lobby Lifecycle — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Two engine instances form a networked 1v1 skirmish through the lobby (host-authoritative config + seed sync, launch barrier) and play a bit-identical lockstep match on localhost/LAN.

**Architecture:** A `MuxTransport` multiplexes a lobby channel and a lockstep channel over one `TcpTransport`. A `LanLobby` host/client handshake syncs a `LanSessionConfig` (scenario + seed) and fires a launch barrier that calls the existing `LaunchSinglePlayerSession`. A new deterministic `SimRandom` (seeded from the host) replaces the non-deterministic weapon RNG so combat stays in sync. Verified by a headless two-process `--lan-host`/`--lan-join` run.

**Tech Stack:** C++17, CMake (VS2022), vcpkg, Catch2, Lua 5.0.

## Global Constraints

- Build: `cmake --preset default` then `cmake --build build --config Debug`. cmake at `C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`; `VCPKG_ROOT=C:\vcpkg` (neither on PATH — use full path / set env in PowerShell).
- Tests: `build/tests/Debug/osc_tests.exe [tag]`. New test files must be added to `tests/CMakeLists.txt` (reconfigure after).
- Single-player must stay bit-identical: no `MuxTransport`/`LanLobby`/sink is created unless the LAN globals are set; the sim uses a fixed default seed.
- Units built from `Unit` in a test TU must `#include "sim/manipulator.hpp"` and `"sim/shield.hpp"` (incomplete-type deleter).
- Namespaces: `osc::sim` for sim pieces, `osc::lua` for lobby pieces. `u8/u16/u32/u64/f32/f64` come from `core/types.hpp`.
- Commit after each task with a `feat:`/`test:` message ending `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`.

---

### Task 1: Deterministic sim RNG

**Files:**
- Create: `src/sim/sim_random.hpp`
- Modify: `src/sim/entity_registry.hpp` (add `SimRandom*` + accessor)
- Modify: `src/sim/sim_state.hpp` (add seed API), `src/sim/sim_state.cpp:32` (wire registry pointer + default seed)
- Modify: `src/sim/weapon.cpp:178-183` (use `registry.sim_random()`)
- Test: `tests/test_sim_rand.cpp`, `tests/CMakeLists.txt`

**Interfaces:**
- Produces: `osc::sim::SimRandom{ void seed(u64); u64 next_u64(); u32 next_u32(); f32 range(f32 lo, f32 hi); u64 state() const; }`; `EntityRegistry::sim_random() -> SimRandom&` + `set_sim_random(SimRandom*)`; `SimState::set_seed(u64)`, `sim_rand()->u32`, `sim_rand_range(f32,f32)->f32`.

- [ ] **Step 1: Write `src/sim/sim_random.hpp`**

```cpp
#pragma once
#include "core/types.hpp"
namespace osc::sim {
// Deterministic SplitMix64 RNG. Lives in sim state so every client fed the same
// seed and the same call sequence produces identical values (lockstep determinism).
class SimRandom {
public:
    explicit SimRandom(u64 seed = 0x9E3779B97F4A7C15ull) : state_(seed) {}
    void seed(u64 s) { state_ = s; }
    u64 next_u64() {
        u64 z = (state_ += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    u32 next_u32() { return static_cast<u32>(next_u64() >> 32); }
    f32 range(f32 lo, f32 hi) {
        f32 unit = static_cast<f32>(next_u64() >> 40) * (1.0f / 16777216.0f); // [0,1)
        return lo + (hi - lo) * unit;
    }
    u64 state() const { return state_; }
private:
    u64 state_;
};
} // namespace osc::sim
```

- [ ] **Step 2: Wire into `EntityRegistry`** — in `src/sim/entity_registry.hpp`, add near the top `#include "sim/sim_random.hpp"`, and inside the class a private `SimRandom default_random_;` + `SimRandom* sim_random_ = &default_random_;` and public `void set_sim_random(SimRandom* r) { sim_random_ = r; }` and `SimRandom& sim_random() { return *sim_random_; }`.

- [ ] **Step 3: Add seed API to `SimState`** — in `sim_state.hpp` (near the RNG-free area, e.g. by `compute_sync_checksum`): a member `SimRandom sim_random_;`, and public `void set_seed(u64 s) { sim_random_.seed(s); }`, `u32 sim_rand() { return sim_random_.next_u32(); }`, `f32 sim_rand_range(f32 lo, f32 hi) { return sim_random_.range(lo, hi); }`. Include `sim/sim_random.hpp`. In `SimState::SimState` (sim_state.cpp:32) body, add `entity_registry_.set_sim_random(&sim_random_);` (the default seed is already set by the member default).

- [ ] **Step 4: Replace weapon RNG** — `src/sim/weapon.cpp:178-183`, replace the `static thread_local std::mt19937 ...` block with:
```cpp
    if (firing_randomness > 0) {
        f32 angle = registry.sim_random().range(-firing_randomness, firing_randomness);
        f32 c = std::cos(angle), s = std::sin(angle);
```
Remove the now-unused `#include <random>` if nothing else in the file uses it (grep first; keep if used).

- [ ] **Step 5: Write `tests/test_sim_rand.cpp`**
```cpp
#include <catch2/catch_test_macros.hpp>
#include "sim/sim_random.hpp"
using osc::sim::SimRandom;
TEST_CASE("SimRandom is deterministic for a given seed", "[simrand]") {
    SimRandom a(12345), b(12345);
    for (int i = 0; i < 100; ++i) REQUIRE(a.next_u64() == b.next_u64());
}
TEST_CASE("SimRandom diverges for different seeds", "[simrand]") {
    SimRandom a(1), b(2);
    bool any_diff = false;
    for (int i = 0; i < 8; ++i) any_diff |= (a.next_u64() != b.next_u64());
    REQUIRE(any_diff);
}
TEST_CASE("SimRandom range stays in bounds and is reseedable", "[simrand]") {
    SimRandom r(7);
    for (int i = 0; i < 1000; ++i) { f32 v = r.range(-3.0f, 3.0f); REQUIRE(v >= -3.0f); REQUIRE(v < 3.0f); }
    SimRandom a(99), b(1); b.seed(99);
    REQUIRE(a.next_u64() == b.next_u64());
}
```
Add `test_sim_rand.cpp` to `tests/CMakeLists.txt`.

- [ ] **Step 6: Build + test** — reconfigure, build `osc_tests`, run `osc_tests.exe "[simrand]"` → PASS; run full `osc_tests.exe` → all pass (weapon change must not break combat tests).

- [ ] **Step 7: Commit** — `feat: add deterministic SimRandom and use it for weapon firing randomness`.

---

### Task 2: MuxTransport

**Files:**
- Create: `src/sim/mux_transport.hpp`
- Test: `tests/test_mux_transport.cpp`, `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `osc::sim::INetTransport`, `LoopbackHub`/`LoopbackTransport` (net_transport.hpp).
- Produces: `osc::sim::MuxTransport{ MuxTransport(std::unique_ptr<INetTransport>); INetTransport& lobby_channel(); INetTransport& game_channel(); void pump(); INetTransport* inner(); }` with `static constexpr u8 TAG_LOBBY=0x4C, TAG_GAME=0x46;`.

- [ ] **Step 1: Write `src/sim/mux_transport.hpp`**
```cpp
#pragma once
#include "sim/net_transport.hpp"
#include <memory>
#include <vector>
namespace osc::sim {
// Multiplexes two logical channels over one INetTransport via a 1-byte tag.
// pump() drains the underlying transport once and sorts messages into per-channel
// inboxes; each channel's receive() returns only its own messages. This keeps
// lobby messages and lockstep frames from ever being parsed as each other.
class MuxTransport {
public:
    static constexpr u8 TAG_LOBBY = 0x4C; // 'L'
    static constexpr u8 TAG_GAME  = 0x46; // 'F'
    explicit MuxTransport(std::unique_ptr<INetTransport> inner)
        : inner_(std::move(inner)),
          lobby_(this, TAG_LOBBY), game_(this, TAG_GAME) {}
    INetTransport& lobby_channel() { return lobby_; }
    INetTransport& game_channel() { return game_; }
    INetTransport* inner() { return inner_.get(); }
    void pump() {
        for (auto& msg : inner_->receive()) {
            if (msg.empty()) continue;
            std::vector<u8> body(msg.begin() + 1, msg.end());
            if (msg[0] == TAG_LOBBY) lobby_.inbox_.push_back(std::move(body));
            else if (msg[0] == TAG_GAME) game_.inbox_.push_back(std::move(body));
        }
    }
private:
    struct Channel : INetTransport {
        Channel(MuxTransport* m, u8 tag) : mux_(m), tag_(tag) {}
        void broadcast(const std::vector<u8>& msg) override {
            std::vector<u8> tagged;
            tagged.reserve(msg.size() + 1);
            tagged.push_back(tag_);
            tagged.insert(tagged.end(), msg.begin(), msg.end());
            mux_->inner_->broadcast(tagged);
        }
        std::vector<std::vector<u8>> receive() override {
            std::vector<std::vector<u8>> out; out.swap(inbox_); return out;
        }
        MuxTransport* mux_; u8 tag_;
        std::vector<std::vector<u8>> inbox_;
    };
    std::unique_ptr<INetTransport> inner_;
    Channel lobby_, game_;
};
} // namespace osc::sim
```

- [ ] **Step 2: Write `tests/test_mux_transport.cpp`**
```cpp
#include <catch2/catch_test_macros.hpp>
#include "sim/mux_transport.hpp"
#include "sim/net_transport.hpp"
#include <memory>
using namespace osc; using osc::sim::MuxTransport; using osc::sim::LoopbackHub;
using osc::sim::LoopbackTransport;
TEST_CASE("MuxTransport routes messages to the correct channel", "[mux]") {
    LoopbackHub hub; int ida = hub.add_endpoint(); int idb = hub.add_endpoint();
    MuxTransport a(std::make_unique<LoopbackTransport>(hub, ida));
    MuxTransport b(std::make_unique<LoopbackTransport>(hub, idb));
    a.lobby_channel().broadcast(std::vector<u8>{1,2,3});
    a.game_channel().broadcast(std::vector<u8>{9,9});
    b.pump();
    auto lob = b.lobby_channel().receive();
    auto gam = b.game_channel().receive();
    REQUIRE(lob.size() == 1); CHECK(lob[0] == std::vector<u8>{1,2,3});
    REQUIRE(gam.size() == 1); CHECK(gam[0] == std::vector<u8>{9,9});
    CHECK(b.lobby_channel().receive().empty()); // drained
}
```
Add to `tests/CMakeLists.txt`.

- [ ] **Step 3: Build + test** — `osc_tests.exe "[mux]"` PASS; full suite PASS.
- [ ] **Step 4: Commit** — `feat: add MuxTransport to multiplex lobby + lockstep on one connection`.

---

### Task 3: LanLobby handshake (transport-only, no Lua yet)

**Files:**
- Create: `src/lua/lan_lobby.hpp`, `src/lua/lan_lobby.cpp`, `src/lua/CMakeLists.txt` (add source)
- Test: `tests/test_lan_lobby.cpp`, `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `MuxTransport` (its `lobby_channel()`).
- Produces:
```cpp
namespace osc::lua {
struct LanSessionConfig { std::string scenario; osc::u64 seed = 0; };
class LanLobby {
public:
    enum class Role { Host, Client };
    enum class State { Idle, Configured, Ready, LaunchReady };
    LanLobby(Role role, osc::sim::INetTransport& lobby_channel);
    void set_host_config(const LanSessionConfig&);   // host: config to advertise
    void request_launch();                            // host: send LAUNCH once Ready
    void poll();                                      // process inbound lobby msgs, advance state
    State state() const;
    Role role() const;
    bool launch_ready() const;                        // both roles: LAUNCH agreed
    const LanSessionConfig& config() const;           // applied/advertised config
};
}
```
State machine — Host: `Idle → (peer READY) Ready → (request_launch) LaunchReady` (sends CONFIG immediately on `set_host_config`, resends on each poll until a READY arrives; sends LAUNCH on request_launch). Client: `Idle → (CONFIG rx) Configured (applies config, sends READY) → (LAUNCH rx) LaunchReady`.

Wire (over lobby channel, little-endian): `u8 type` then payload. `1=CONFIG {u64 seed, u32 len, bytes scenario}`, `2=READY {}`, `3=LAUNCH {}`.

- [ ] **Step 1: Write the failing test `tests/test_lan_lobby.cpp`** — drive host+client over two `MuxTransport`s on a `LoopbackHub`, pumping both muxes each round:
```cpp
#include <catch2/catch_test_macros.hpp>
#include "lua/lan_lobby.hpp"
#include "sim/mux_transport.hpp"
#include "sim/net_transport.hpp"
#include <memory>
using namespace osc; using osc::lua::LanLobby; using osc::lua::LanSessionConfig;
using osc::sim::MuxTransport; using osc::sim::LoopbackHub; using osc::sim::LoopbackTransport;
TEST_CASE("LanLobby host/client reach launch with synced config", "[lanlobby]") {
    LoopbackHub hub; int ih = hub.add_endpoint(); int ic = hub.add_endpoint();
    MuxTransport hm(std::make_unique<LoopbackTransport>(hub, ih));
    MuxTransport cm(std::make_unique<LoopbackTransport>(hub, ic));
    LanLobby host(LanLobby::Role::Host, hm.lobby_channel());
    LanLobby client(LanLobby::Role::Client, cm.lobby_channel());
    host.set_host_config(LanSessionConfig{"/maps/x/x_scenario.lua", 0xABCDEF12u});
    for (int r = 0; r < 20 && !(host.launch_ready() && client.launch_ready()); ++r) {
        hm.pump(); cm.pump();
        host.poll(); client.poll();
        if (host.state() == LanLobby::State::Ready) host.request_launch();
    }
    REQUIRE(client.state() == LanLobby::State::LaunchReady);
    REQUIRE(host.launch_ready());
    REQUIRE(client.launch_ready());
    CHECK(client.config().scenario == "/maps/x/x_scenario.lua");
    CHECK(client.config().seed == 0xABCDEF12u);
}
```
Add to `tests/CMakeLists.txt`.

- [ ] **Step 2: Run to confirm it fails** (link error / no LanLobby).

- [ ] **Step 3: Implement `lan_lobby.hpp` + `lan_lobby.cpp`** with the interface above. Serialization mirrors `lockstep_session.cpp`'s little-endian `put_u32`/`Reader` style (u64 = two u32, low then high). Host `poll()`: on each call, if not yet Ready, (re)broadcast CONFIG; when a READY arrives set state Ready. `request_launch()`: broadcast LAUNCH, set LaunchReady. Client `poll()`: on CONFIG → store config, broadcast READY, state Configured; on LAUNCH → state LaunchReady. `launch_ready()` = state==LaunchReady. Add `lan_lobby.cpp` to `src/lua/CMakeLists.txt`.

- [ ] **Step 4: Build + test** — `osc_tests.exe "[lanlobby]"` PASS; full suite PASS.
- [ ] **Step 5: Commit** — `feat: add LanLobby host/client handshake (config + seed sync, launch barrier)`.

---

### Task 4: Integrate lobby + mux + seed into mp_net_state and the game loop

**Files:**
- Modify: `src/lua/mp_net_state.hpp`/`.cpp` (own `MuxTransport` + `LanLobby` + `seed`; session over game channel; expose `lan_lobby()`, `mp_pump()`)
- Modify: `src/lua/moho_bindings.cpp` (HostGame/JoinGame create mux+lobby)
- Modify: `src/main.cpp` (game loop: pump lobby in FRONT_END, pump mux in the MP tick branch; apply seed at launch)

**Interfaces:**
- Consumes: Task 1 `set_seed`, Task 2 `MuxTransport`, Task 3 `LanLobby`.
- Produces: `osc::lua::mp_net_state()` gains `std::unique_ptr<osc::sim::MuxTransport> mux; std::unique_ptr<LanLobby> lobby; u64 seed = <default>;`. Helpers: `void mp_pump()` (calls `mux->pump()` + host `poll_connections`), `LanLobby* mp_lobby()`. `mp_attach_session(sim)` builds the `LockstepSession` over `mux->game_channel()` and calls `sim.set_seed(seed)`.

- [ ] **Step 1: mp_net_state owns mux + lobby + seed** — in `mp_begin_host`/`mp_begin_join`, after creating the `TcpTransport` (keep `host_tcp` alias), wrap it: `s.mux = std::make_unique<MuxTransport>(std::move(t)); s.lobby = std::make_unique<LanLobby>(role, s.mux->lobby_channel());`. Store `s.transport` as before OR drop it in favor of `mux->inner()` — keep `host_tcp` for `poll_connections`. Add `u64 seed`.

- [ ] **Step 2: `mp_attach_session` over the game channel** — change `LockstepSession(sim, *s.transport, ...)` to `LockstepSession(sim, s.mux->game_channel(), ...)`; then `sim.set_seed(s.seed);`. Keep the sink install.

- [ ] **Step 3: `mp_pump()` + teardown order** — add `void mp_pump()` = `if (s.mux) s.mux->pump(); mp_poll_connections();`. In `mp_teardown`/`reset`: destroy `lobby` → `session` → `mux` (which owns the transport) → null `host_tcp`.

- [ ] **Step 4: HostGame/JoinGame** — unchanged except they now cause `mp_begin_host/join` to also build mux+lobby (already in Step 1). For host, after `mp_begin_host`, `mp_lobby()->set_host_config(...)` is set by the CLI/launch path (Task 5), not here.

- [ ] **Step 5: Game loop** — in `main.cpp` FRONT_END path (each frame, when `mp_net_state().lobby` exists and not yet launched): `mp_pump(); auto* lob = mp_lobby(); lob->poll(); if (host && lob->state()==Ready) lob->request_launch(); if (lob->launch_ready() && !launch_fired) { build sessionConfig from lob->config(); LaunchSinglePlayerSession(...); }`. In the MP tick branch, add `mp_pump();` before `session->send_frame()`.

- [ ] **Step 6: Build** — full build; run full `osc_tests` (no behavior change expected for SP) + SP `--full-smoke-test` clean.
- [ ] **Step 7: Commit** — `feat: drive LAN lobby + mux + shared seed through mp_net_state and the game loop`.

---

### Task 5: CLI entry + headless two-process verification

**Files:**
- Modify: `src/main.cpp` (parse `--lan-host`/`--lan-join`; set lobby globals; a headless `run_lan_lobby_test`; a `build_1v1_session_config` helper adapting the `--auto-skirmish` construction at ~main.cpp:1458)
- Modify: `docs/current-state.md` (record the LAN lobby verification)

**Interfaces:**
- Consumes: everything above; existing `LaunchSinglePlayerSession`, `execute_reload_sequence`, the `--auto-skirmish` sessionConfig construction.

- [ ] **Step 1: CLI flags** — parse `--lan-host [port]` → set global `__osc_mp_host_port`; `--lan-join <ip> [port]` → set `__osc_mp_join_address` + `__osc_mp_join_port`. These feed the existing HostGame/JoinGame wiring.

- [ ] **Step 2: `build_1v1_session_config(lua_State* uL, const std::string& scenario, u64 seed)`** — push a sessionConfig table with `GameOptions.ScenarioFile = scenario` and `PlayerOptions` = two HUMAN slots (slot1, slot2), factions UEF/Aeon, distinct colors/teams, `Human=true`. Model on the `--auto-skirmish` construction (main.cpp ~1458) but 2 humans, no AI. Return it on the stack for `LaunchSinglePlayerSession`.

- [ ] **Step 3: `run_lan_lobby_test(bool is_host, ip, port, frames)`** — a headless mode that boots the full engine (ui_L + sim reload path) like the windowed loop but no renderer:
  1. Set the LAN globals; boot front-end; call `HostGame`/`JoinGame` (creates transport+mux+lobby).
  2. Host: `mp_lobby()->set_host_config({scenario, seed})`. Loop pumping `mp_pump()` + `lobby->poll()` until `launch_ready()` (host auto-`request_launch()` at Ready).
  3. On launch: `build_1v1_session_config` + `LaunchSinglePlayerSession` → `execute_reload_sequence` (headless, renderer=nullptr) → `mp_attach_session` (game channel + seed).
  4. Run a bounded tick loop via the session (`mp_pump(); send_frame(); receive_and_advance()`), issuing one scripted move + letting a weapon fire; every few ticks record `compute_sync_checksum()`.
  5. Print `LAN_RESULT role=… tick=… checksum=… desynced=…`; return non-zero on desync/stall.
  Dispatch it early in `main()` (like `run_mp_lan_test`).

- [ ] **Step 4: Build** — full build clean.

- [ ] **Step 5: Two-process verification** — terminal A `opensupcom --lan-host --mp-port 47900 --mp-frames 40`; terminal B `opensupcom --lan-join 127.0.0.1 --mp-port 47900 --mp-frames 40`. Expect both: same `checksum`, `desynced=0`. If the real scenario tick hangs (AI), fall back to a no-AI 1v1 and/or assert the post-launch initial checksum matches; document the outcome.

- [ ] **Step 6: Docs + commit** — update `docs/current-state.md`; commit `feat: --lan-host/--lan-join windowed LAN lobby entry + headless two-process verification`.

---

## Self-review notes
- Spec coverage: RNG (T1), MuxTransport (T2), LanLobby+launch barrier (T3), integration/game-loop/seed-apply (T4), CLI+windowed reachability+headless verification (T5). All spec components mapped.
- Risk (scenario tick hang) is handled in T5 Step 5 with explicit fallbacks.
- Types consistent across tasks: `LanSessionConfig{scenario,seed}`, `MuxTransport::{lobby,game}_channel()`, `SimState::set_seed`, `EntityRegistry::sim_random()`.
