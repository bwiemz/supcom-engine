# LAN IP-Entry UI Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a front-end "LAN Game" button + IP/Host/Join dialog that starts or joins a LAN game through new `LanHost()`/`LanJoin(ip)` engine globals over the existing (verified) LAN lobby lifecycle.

**Architecture:** Two thin Lua-callable engine globals wrap the already-verified `mp_begin_host`/`mp_begin_join`. A small `pcall`-guarded Lua snippet, run on `ui_L` after the front-end `CreateUI()`, builds a "LAN Game" button that opens a dialog (IP `Edit` + Host/Join/Close + status). The globals are fully headless-verified; the dialog is the one untestable-in-CI inch (no GUI window automation).

**Tech Stack:** C++17, CMake (VS2022), Lua 5.0, FA `maui`/`UIUtil` UI framework.

## Global Constraints

- Build: `cmake --build build --config Debug` (cmake at `C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`, `VCPKG_ROOT=C:\vcpkg`, neither on PATH).
- Engine globals go in `src/lua/moho_bindings.cpp` (namespace `osc::lua`), registered via `LuaState::register_function`. `mp_net_state.hpp` is already included there.
- Lua is 5.0: no `lua_getfield`; use `lua_tostring`/`lua_isstring` carefully (`lua_type(L,i)==LUA_TSTRING`), `luaL_optnumber` for optional numeric args.
- `rawset(_G, ...)` bypasses config.lua's global lock (the LAN globals are registered before config lock via register_function, so they're fine; the dialog snippet uses normal globals it defines with rawset if needed).
- Single-player must stay unaffected: the dialog snippet runs only on the real windowed front end; the globals are inert until called.
- Commit after each task; message ends `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`.

---

### Task 1: Engine globals `LanHost` / `LanJoin` / `LanNetStatus` + headless verification

**Files:**
- Modify: `src/lua/moho_bindings.cpp` (add 3 static C functions + a `register_lan_ui_bindings(LuaState&)` helper; call it from `register_ui_bindings`)
- Modify: `src/lua/moho_bindings.hpp` (declare `register_lan_ui_bindings`)
- Modify: `src/main.cpp` (a `--lan-ui-test` headless mode dispatched next to `--lan-host`)

**Interfaces:**
- Consumes: `osc::lua::mp_begin_host(u16)`, `mp_begin_join(const std::string&, u16)`, `mp_teardown()`, `mp_net_state()` (from mp_net_state.hpp, already included).
- Produces: Lua globals `LanHost([port]) -> bool`, `LanJoin(ip[, port]) -> bool`, `LanNetStatus() -> string`; C++ `void osc::lua::register_lan_ui_bindings(LuaState&)`.

- [ ] **Step 1: Add the three static C functions in `moho_bindings.cpp`** (place them just above `register_ui_bindings`, ~line 14950):

```cpp
// LanHost([port]) -> bool : start hosting a LAN game (default port 47624).
static int l_LanHost(lua_State* L) {
    if (mp_net_state().transport_ready) { lua_pushboolean(L, 0); return 1; }
    auto port = static_cast<u16>(
        lua_isnumber(L, 1) ? static_cast<u16>(lua_tonumber(L, 1)) : 47624);
    bool ok = mp_begin_host(port);
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

// LanJoin(ip[, port]) -> bool : connect to a LAN host at ip[:port].
static int l_LanJoin(lua_State* L) {
    if (mp_net_state().transport_ready) { lua_pushboolean(L, 0); return 1; }
    if (lua_type(L, 1) != LUA_TSTRING) { lua_pushboolean(L, 0); return 1; }
    std::string ip = lua_tostring(L, 1);
    // trim spaces; reject empty
    size_t a = ip.find_first_not_of(" \t");
    size_t b = ip.find_last_not_of(" \t");
    ip = (a == std::string::npos) ? std::string() : ip.substr(a, b - a + 1);
    if (ip.empty()) { lua_pushboolean(L, 0); return 1; }
    auto port = static_cast<u16>(
        lua_isnumber(L, 2) ? static_cast<u16>(lua_tonumber(L, 2)) : 47624);
    bool ok = mp_begin_join(ip, port);
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

// LanNetStatus() -> string : short status for the dialog to display.
static int l_LanNetStatus(lua_State* L) {
    auto& s = mp_net_state();
    const char* status;
    if (s.session) status = "in game";
    else if (!s.transport_ready) status = "idle";
    else if (s.role == MpNetState::Role::Host) status = "hosting: waiting for player";
    else status = "connecting";
    lua_pushstring(L, status);
    return 1;
}

void register_lan_ui_bindings(LuaState& state) {
    state.register_function("LanHost", l_LanHost);
    state.register_function("LanJoin", l_LanJoin);
    state.register_function("LanNetStatus", l_LanNetStatus);
}
```

- [ ] **Step 2: Call the helper from `register_ui_bindings`** — add `register_lan_ui_bindings(state);` inside `register_ui_bindings` (after the other `register_function` calls, ~line 14990).

- [ ] **Step 3: Declare the helper in `moho_bindings.hpp`** — next to `register_ui_bindings`:
```cpp
void register_lan_ui_bindings(LuaState& state);
```

- [ ] **Step 4: Add the `--lan-ui-test` headless mode in `main.cpp`** — dispatch it in the early MP/LAN block (next to `--lan-host`), before engine init:
```cpp
        if (parse_flag(argc, argv, "--lan-ui-test")) {
            osc::lua::LuaState uiL;
            osc::lua::register_lan_ui_bindings(uiL);
            auto& mp = osc::lua::mp_net_state();
            mp.reset();
            int fails = 0;
            // LanHost() creates a listening transport.
            uiL.do_string("__r = LanHost()");
            uiL.do_string("__r_host = __r");
            if (!mp.transport_ready) { spdlog::error("[lan-ui] LanHost did not create transport"); fails++; }
            else spdlog::info("[lan-ui] LanHost OK (port {})", mp.port);
            osc::lua::mp_teardown();
            // LanJoin("") is rejected, no transport.
            uiL.do_string("__rj = LanJoin('')");
            bool rj = false;
            { lua_State* L = uiL.raw(); lua_getglobal(L, "__rj"); rj = lua_toboolean(L, -1) != 0; lua_pop(L, 1); }
            if (rj || mp.transport_ready) { spdlog::error("[lan-ui] LanJoin(empty) not rejected"); fails++; }
            else spdlog::info("[lan-ui] LanJoin(empty) correctly rejected");
            osc::lua::mp_teardown();
            std::printf("LAN_UI_TEST fails=%d\n", fails);
            std::fflush(stdout);
            return fails == 0 ? 0 : 1;
        }
```
(`osc::lua::LuaState` is already used throughout `main.cpp`; `#include "lua/lua_state.hpp"` is present. `mp_net_state.hpp` and `moho_bindings.hpp` are already included.)

- [ ] **Step 5: Build** — `cmake --build build --config Debug` → EXIT 0 (both targets).

- [ ] **Step 6: Run the headless binding test** — `build/Debug/opensupcom.exe --lan-ui-test` → prints `LAN_UI_TEST fails=0`, exit 0. Confirms `LanHost` creates a transport and `LanJoin("")` is rejected.

- [ ] **Step 7: Commit** — `feat: LanHost/LanJoin/LanNetStatus engine globals + --lan-ui-test`.

---

### Task 2: LAN dialog UI snippet + front-end wiring

**Files:**
- Create: `src/lua/lan_dialog_ui.hpp` (a `const char*` with the Lua snippet, kept out of main.cpp for readability)
- Modify: `src/main.cpp` (run the snippet after the two front-end `CreateUI()` calls)

**Interfaces:**
- Consumes: Task 1 globals `LanHost`/`LanJoin`/`LanNetStatus`; FA `UIUtil`, `import('/lua/maui/edit.lua')`, `LayoutHelpers`, `GetFrame(0)`.
- Produces: `osc::lua::kLanDialogLua` (the snippet string).

- [ ] **Step 1: Create `src/lua/lan_dialog_ui.hpp`** with the snippet. It is entirely wrapped in `pcall` so any FA-UI API mismatch logs a warning instead of breaking the front end:

```cpp
#pragma once
namespace osc::lua {
// Front-end "LAN Game" button + IP/Host/Join dialog, run on ui_L after CreateUI().
// Fully pcall-guarded: a UI error logs and leaves the menu usable. Calls the
// LanHost/LanJoin/LanNetStatus engine globals (registered via register_ui_bindings).
inline constexpr const char* kLanDialogLua = R"LUA(
local ok, err = pcall(function()
    if rawget(_G, '__osc_lan_dialog_built') then return end
    rawset(_G, '__osc_lan_dialog_built', true)
    local root = GetFrame(0)
    if not root then return end
    local Edit = import('/lua/maui/edit.lua').Edit

    -- The "LAN Game" launcher button, placed top-left of the menu.
    local lanBtn = UIUtil.CreateButtonStd(root, '/widgets/small', 'LAN Game', 16)
    LayoutHelpers.AtLeftTopIn(lanBtn, root, 40, 40)

    -- The dialog group (hidden until the button is clicked).
    local dlg = Group(root)
    dlg.Width:Set(360); dlg.Height:Set(220)
    LayoutHelpers.AtCenterIn(dlg, root)
    local bg = Bitmap(dlg)
    bg:SetSolidColor('dd101018'); LayoutHelpers.FillParent(bg, dlg)
    local title = UIUtil.CreateText(dlg, 'LAN Game', 20, UIUtil.bodyFont)
    LayoutHelpers.AtTopIn(title, dlg, 12); LayoutHelpers.AtHorizontalCenterIn(title, dlg)
    local ipEdit = Edit(dlg)
    ipEdit.Width:Set(240); ipEdit.Height:Set(28)
    LayoutHelpers.AtTopIn(ipEdit, dlg, 60); LayoutHelpers.AtHorizontalCenterIn(ipEdit, dlg)
    if ipEdit.SetText then ipEdit:SetText('127.0.0.1') end
    local status = UIUtil.CreateText(dlg, 'idle', 14, UIUtil.bodyFont)
    LayoutHelpers.AtBottomIn(status, dlg, 12); LayoutHelpers.AtHorizontalCenterIn(status, dlg)

    local hostBtn = UIUtil.CreateButtonStd(dlg, '/widgets/small', 'Host', 14)
    LayoutHelpers.AtTopIn(hostBtn, dlg, 110); LayoutHelpers.AtLeftIn(hostBtn, dlg, 40)
    local joinBtn = UIUtil.CreateButtonStd(dlg, '/widgets/small', 'Join', 14)
    LayoutHelpers.AtTopIn(joinBtn, dlg, 110); LayoutHelpers.AtRightIn(joinBtn, dlg, 40)
    local closeBtn = UIUtil.CreateButtonStd(dlg, '/widgets/small', 'Close', 14)
    LayoutHelpers.AtBottomIn(closeBtn, dlg, 40); LayoutHelpers.AtHorizontalCenterIn(closeBtn, dlg)

    dlg:Hide()
    lanBtn.OnClick = function() dlg:Show() end
    closeBtn.OnClick = function() dlg:Hide() end
    hostBtn.OnClick = function()
        if LanHost() then status:SetText('hosting: waiting for player') else status:SetText('host failed') end
    end
    joinBtn.OnClick = function()
        local ip = ipEdit.GetText and ipEdit:GetText() or '127.0.0.1'
        if LanJoin(ip) then status:SetText('connecting to ' .. ip) else status:SetText('join failed') end
    end
    -- Refresh status each beat while the dialog is up.
    dlg.OnFrame = function(self, delta)
        if self:IsHidden() then return end
        status:SetText(LanNetStatus())
    end
end)
if not ok then LOG('[lan-ui] dialog build failed: ' .. tostring(err)) end
)LUA";
} // namespace osc::lua
```

- [ ] **Step 2: Include + run the snippet after the front-end `CreateUI()`** — in `main.cpp` add `#include "lua/lan_dialog_ui.hpp"` near the other lua includes, then after the windowed front-end `CreateUI()` call (~line 1798) add:
```cpp
            {
                auto r = ui_lua_state.do_string(osc::lua::kLanDialogLua);
                if (!r) spdlog::warn("LAN dialog UI error: {}", r.error().message);
            }
```

- [ ] **Step 3: Also run it after ReturnToLobby's `CreateUI`** — find the return-to-lobby handler's `osc::core::call_lua_global(uiL, "CreateUI")` and, right after it, reset the build guard + re-run the snippet:
```cpp
                        // Rebuild the LAN dialog on the fresh front end.
                        {
                            lua_pushstring(uiL, "__osc_lan_dialog_built");
                            lua_pushnil(uiL);
                            lua_rawset(uiL, LUA_GLOBALSINDEX);
                            auto r = ui_lua_state.do_string(osc::lua::kLanDialogLua);
                            if (!r) spdlog::warn("LAN dialog UI (relobby) error: {}", r.error().message);
                        }
```

- [ ] **Step 4: Build** — `cmake --build build --config Debug` → EXIT 0.

- [ ] **Step 5: Regression — full suite + SP smoke** — `osc_tests.exe` all pass; `opensupcom.exe --full-smoke-test --map "/maps/SCMP_009/SCMP_009_scenario.lua"` → smoke report 0 issues (the snippet does not run in `--full-smoke-test`, which drives its own UI; this confirms no build/link regression and SP is intact). Also re-run `--lan-ui-test` → `fails=0` and the two-process `--lan-host`/`--lan-join` → still synced.

- [ ] **Step 6: Docs + commit** — note the dialog is best-effort/window-only (rendering unverified in CI, pcall-guarded) in `docs/current-state.md`; commit `feat: front-end LAN Game button + IP/Host/Join dialog`.

---

## Self-review notes
- Spec coverage: engine globals (T1), dialog + wiring (T2), headless binding test (T1 Step 6), regression + honest limit (T2 Step 5). All spec sections mapped.
- The dialog (T2) is explicitly the untestable inch; it is pcall-guarded so a wrong FA-UI signature degrades to a logged warning, never a crash or an SP regression. If window testing later shows an API mismatch, only the snippet string changes.
- Type/name consistency: `LanHost`/`LanJoin`/`LanNetStatus`, `register_lan_ui_bindings`, `kLanDialogLua`, `mp_net_state()` fields (`transport_ready`, `role`, `session`) all match Task 1 and the existing mp_net_state.hpp.
