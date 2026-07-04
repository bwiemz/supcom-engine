# LAN IP-Entry UI — Design

Date: 2026-07-04
Status: approved, ready for implementation plan
Depends on: windowed LAN lobby lifecycle (PR #15) — `mp_begin_host`/`mp_begin_join`,
`mp_net_state`, the game-loop lobby-driving block, `LanLobby`.

## Goal

Let a player start or join a LAN game from the running window: a "LAN Game" button
on the front-end menu opens a small dialog with a host-IP text field and Host / Join
buttons. The buttons call the already-verified LAN engine functions; from there the
existing game-loop handshake → launch path runs unchanged. This is the UI→engine
entry the LAN lobby lifecycle was missing (previously only reachable via the
`--lan-window-host/join` CLI flags).

### In scope
- Two engine globals callable from Lua UI: `LanHost([port])`, `LanJoin(ip[, port])`,
  plus `LanNetStatus()` for a progress/status string.
- A front-end "LAN Game" button that opens a dialog: IP `Edit` field, Host button,
  Join button, Close button, and a status line.
- Headless verification of the UI→engine binding.

### Out of scope (documented follow-ups)
Server browser / LAN discovery, an editable port field, map/faction pickers in the
dialog, styling polish, >2 players.

## Architecture

```
front-end menu ── "LAN Game" button ──▶ LAN dialog (Group)
                                          │ IP Edit + Host/Join/Close + status Text
                                          ▼
                        LanHost()/LanJoin(ip)  (engine globals)
                                          ▼
                        mp_begin_host / mp_begin_join   (existing, verified)
                                          ▼
        game-loop lobby-driving block → LaunchSinglePlayerSession → lockstep
```

No sim/networking changes. The dialog is a thin Lua layer over the existing LAN
engine functions; nothing past the button click is new.

## Component 1 — Engine globals

Registered in `moho_bindings.cpp` `register_ui_bindings` (ui_L), alongside the other
UI globals:

- `LanHost([port]) -> bool` — calls `osc::lua::mp_begin_host(port)` (default port
  47624). Returns success. On success the game-loop block will advertise the host
  config and fire the launch barrier once a client readies.
- `LanJoin(ip[, port]) -> bool` — validates `ip` is a non-empty string; calls
  `osc::lua::mp_begin_join(ip, port)`. Returns success.
- `LanNetStatus() -> string` — derives a short status from `mp_net_state()`:
  `"idle"` (no transport), `"hosting: waiting for player"` (host, no session yet),
  `"connecting"` (client, no session yet), `"in game"` (session active), or
  `"error"` is surfaced by the boolean return of Host/Join rather than here (a
  failed begin leaves state idle). Read-only; safe to call every beat.

These are thin wrappers — the heavy lifting (`TcpTransport` + `MuxTransport` +
`LanLobby`) already exists and is verified. Guard: they no-op / return false if a
transport already exists (`mp_net_state().transport_ready`), so a double-click can't
create two transports.

## Component 2 — LAN dialog (Lua UI)

An engine-owned Lua snippet run on `ui_L` right after
`import('/lua/ui/menus/main.lua').CreateUI()` on the front end. Delivered as an
embedded `do_string` (the engine cannot add files under FA's `/lua` VFS tree). It
uses FA's `maui` classes (`Button`, `Edit`, `Group`, `Text`, `Bitmap`) — the same
ones the lobby uses and the engine already renders.

- Creates a "LAN Game" `Button` parented to the front-end root, positioned near the
  existing Skirmish button (absolute LayoutHelpers placement; it need not be part of
  FA's internal menu list).
- Its `OnClick` builds/shows a `Group` dialog (hidden until then) containing:
  - a title `Text` "LAN Game",
  - an `Edit` for the host IP (prefilled "127.0.0.1"),
  - a "Host" `Button` → `LanHost()`,
  - a "Join" `Button` → `LanJoin(edit:GetText())`,
  - a "Close" `Button` → hide the dialog,
  - a status `Text`, refreshed each beat (an `OnFrame`/beat handler) from
    `LanNetStatus()`.
- On launch, the state machine transitions FRONT_END→LOADING and the front-end UI is
  torn down, so the dialog disappears naturally.

Kept small and defensive: every FA-UI call that might be missing a field is guarded
(`pcall` around the whole builder), so a UI hiccup logs a warning rather than
crashing the front end.

## Component 3 — Wiring the snippet in

`main.cpp` runs the LAN-dialog `do_string` on `ui_L` immediately after the existing
front-end `CreateUI()` call (both in the windowed boot and in `ReturnToLobby`'s
`CreateUI`, so it reappears after a game). Gated so it only runs on the real
windowed front end (not in headless test modes that construct their own UI), and
wrapped in the LuaState error check already used for `CreateUI`.

## Testing

- **Headless binding test** — a `--lan-ui-test` main mode (the globals need a real
  `lua_State` with the UI bindings + `mp_net_state`, which is simplest to stand up in
  `main.cpp` rather than a bare Catch2 fixture). It opens a `lua_State`, registers the
  globals, then:
  - call `LanHost()` → assert `mp_net_state().transport_ready == true` and a
    listening transport exists; then `mp_teardown()`.
  - call `LanJoin("")` → assert it returns false and no transport was created.
  - call `LanJoin("127.0.0.1", 1)` to a dead port → assert it returns false
    gracefully (no crash), state left idle.
  This proves the UI→engine binding and the guards. `LanJoin` success needs a live
  host and is already covered by the two-process `--lan-host`/`--lan-join` run.
- **Regression**: full `osc_tests` green; `--full-smoke-test` clean (the dialog only
  loads on the windowed front end and only acts on click; SP is untouched).
- **Honest limit**: clicking the rendered button / typing in the field in a live
  window is the one untestable inch (no two-GUI-window automation here). The binding,
  the guards, and load-without-error are covered; the visual click is verified only
  by logic-equivalence to the CLI path.

## Error handling

- `LanHost`/`LanJoin` return false on socket failure; the dialog shows the failure
  and stays open for retry. Never affects single-player.
- Empty IP on Join → false, status prompts for an IP, no connect attempt.
- Double-click / already-connected → guarded no-op (transport_ready check).
- The dialog builder runs under `pcall`; a UI error logs and leaves the menu usable.

## Success criteria

- New binding test + full suite green; `--full-smoke-test` clean.
- `LanHost()` from Lua creates a listening transport; `LanJoin("")` is rejected.
- The two-process `--lan-host`/`--lan-join` LAN match still syncs (unchanged).
- Single-player behavior unchanged.
