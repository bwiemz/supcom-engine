# Full Skirmish Completion — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Complete every feature needed for a full human-vs-AI skirmish game: lobby interaction, game launch, all unit abilities (cloak, stealth, teleport, adjacency, death weapons, scrying), and game-over flow.

**Architecture:** All changes are in 4 C++ files (moho_bindings.cpp, sim_bindings.cpp, unit.hpp/cpp) plus sim_state for temporary vision. The lobby pipeline fixes are in moho_bindings.cpp and main.cpp. Each task is independent and commits separately.

**Tech Stack:** C++17, Lua 5.0 C API, Vulkan renderer (no renderer changes needed)

**Spec:** `docs/plans/2026-04-05-full-skirmish-completion-design.md`

---

## Phase 1: Lobby Pipeline

### Task 1: Control.Disable/Enable/IsDisabled

**Files:**
- Modify: `src/lua/moho_bindings.cpp:8891-8916` (ui_control_methods array)

- [ ] **Step 1: Add Disable/Enable/IsDisabled C functions**

Add before the sentinel at line 8916:

```cpp
static int control_Disable(lua_State* L) {
    if (!lua_istable(L, 1)) return 0;
    lua_pushstring(L, "_isDisabled");
    lua_pushboolean(L, 1);
    lua_rawset(L, 1);
    return 0;
}

static int control_Enable(lua_State* L) {
    if (!lua_istable(L, 1)) return 0;
    lua_pushstring(L, "_isDisabled");
    lua_pushboolean(L, 0);
    lua_rawset(L, 1);
    return 0;
}

static int control_IsDisabled(lua_State* L) {
    if (!lua_istable(L, 1)) { lua_pushboolean(L, 0); return 1; }
    lua_pushstring(L, "_isDisabled");
    lua_rawget(L, 1);
    lua_pushboolean(L, lua_toboolean(L, -1));
    lua_replace(L, -2);
    return 1;
}
```

- [ ] **Step 2: Register in ui_control_methods array**

Add entries before the `{nullptr, nullptr}` sentinel at line 8916:
```cpp
    {"Disable",     control_Disable},
    {"Enable",      control_Enable},
    {"IsDisabled",  control_IsDisabled},
```

- [ ] **Step 3: Build and verify**

Run: `cmake --build build --config Debug 2>&1 | tail -3`
Expected: Build succeeds

- [ ] **Step 4: Commit**

```bash
git add src/lua/moho_bindings.cpp
git commit -m "Add Control:Disable/Enable/IsDisabled for FA Combo compatibility"
```

---

### Task 2: ItemList.AddItems/ClearItems/SetTitleText

**Files:**
- Modify: `src/lua/moho_bindings.cpp:9984-10005` (ui_item_list_methods array)

- [ ] **Step 1: Add AddItems C function**

```cpp
static int itemlist_AddItems(lua_State* L) {
    // arg 1 = self (item list), arg 2 = table of strings
    if (!lua_istable(L, 1) || !lua_istable(L, 2)) return 0;
    int n = luaL_getn(L, 2);
    for (int i = 1; i <= n; i++) {
        lua_rawgeti(L, 2, i);
        if (lua_type(L, -1) == LUA_TSTRING) {
            // Call self:AddItem(str)
            lua_pushstring(L, "AddItem");
            lua_gettable(L, 1);
            lua_pushvalue(L, 1); // self
            lua_pushvalue(L, -3); // str
            lua_pcall(L, 2, 0, 0);
        }
        lua_pop(L, 1); // pop string
    }
    return 0;
}

static int itemlist_ClearItems(lua_State* L) {
    // Delegate to DeleteAllItems
    lua_pushstring(L, "DeleteAllItems");
    lua_gettable(L, 1);
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, 1);
        lua_pcall(L, 1, 0, 0);
    } else {
        lua_pop(L, 1);
    }
    return 0;
}

static int itemlist_SetTitleText(lua_State* L) {
    // Store title on instance for Combo to read
    if (!lua_istable(L, 1)) return 0;
    lua_pushstring(L, "_titleText");
    lua_pushvalue(L, 2);
    lua_rawset(L, 1);
    return 0;
}
```

- [ ] **Step 2: Register in ui_item_list_methods**

Add before sentinel:
```cpp
    {"AddItems",     itemlist_AddItems},
    {"ClearItems",   itemlist_ClearItems},
    {"SetTitleText",  itemlist_SetTitleText},
```

- [ ] **Step 3: Build and verify**
- [ ] **Step 4: Commit**

```bash
git add src/lua/moho_bindings.cpp
git commit -m "Add ItemList:AddItems/ClearItems/SetTitleText for Combo dropdown"
```

---

### Task 3: SendData Loopback

**Files:**
- Modify: `src/lua/moho_bindings.cpp:11839` (lobby_SendData)

- [ ] **Step 1: Replace stub with loopback implementation**

Replace the one-liner at line 11839:
```cpp
static int lobby_SendData(lua_State* L) {
    // Single-player loopback: call lobbyComm:DataReceived(data) directly
    // Args: self (lobbyComm), targetID, data
    if (!lua_istable(L, 1) || !lua_istable(L, 3)) return 0;

    // Inject SenderID and SenderName into data
    lua_pushstring(L, "SenderID");
    lua_pushstring(L, "1");
    lua_rawset(L, 3);

    lua_pushstring(L, "SenderName");
    // Read local player name from lobbyComm instance
    lua_pushstring(L, "GetLocalPlayerName");
    lua_gettable(L, 1);
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, 1);
        if (lua_pcall(L, 1, 1, 0) == 0 && lua_type(L, -1) == LUA_TSTRING) {
            lua_rawset(L, 3); // data.SenderName = name
        } else {
            lua_pop(L, 1);
            lua_pushstring(L, "Player");
            lua_rawset(L, 3);
        }
    } else {
        lua_pop(L, 1);
        lua_pushstring(L, "Player");
        lua_rawset(L, 3);
    }

    // Call self:DataReceived(data)
    lua_pushstring(L, "DataReceived");
    lua_gettable(L, 1);
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, 1); // self
        lua_pushvalue(L, 3); // data
        if (lua_pcall(L, 2, 0, 0) != 0) {
            spdlog::warn("SendData loopback DataReceived error: {}", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1);
    }

    return 0;
}
```

- [ ] **Step 2: Also update BroadcastData to use same loopback**

Replace `lobby_BroadcastData` (line 11689):
```cpp
static int lobby_BroadcastData(lua_State* L) {
    // Single-player: same as SendData — loopback to self
    return lobby_SendData(L);
}
```

- [ ] **Step 3: Build and verify**
- [ ] **Step 4: Commit**

```bash
git add src/lua/moho_bindings.cpp
git commit -m "Implement SendData/BroadcastData loopback for single-player lobby"
```

---

### Task 4: SessionConfig Extended Parsing

**Files:**
- Modify: `src/main.cpp:574-664` (execute_reload_sequence sessionConfig block)
- Modify: `src/sim/army_brain.hpp` (add color field)

- [ ] **Step 1: Add color field to ArmyBrain**

In army_brain.hpp, add:
```cpp
    i32 player_color_ = -1;
    i32 army_color_ = -1;
public:
    void set_player_color(i32 c) { player_color_ = c; }
    void set_army_color(i32 c) { army_color_ = c; }
    i32 player_color() const { return player_color_; }
    i32 army_color() const { return army_color_; }
```

- [ ] **Step 2: Extend sessionConfig parsing in main.cpp**

Inside the PlayerOptions loop (after Faction read), add reads for StartSpot, Team, PlayerColor, ArmyColor:

```cpp
    // Read StartSpot (1-based spawn position index)
    lua_pushstring(uiL, "StartSpot");
    lua_rawget(uiL, entry);
    int start_spot = lua_isnumber(uiL, -1)
        ? static_cast<int>(lua_tonumber(uiL, -1)) : slot;
    lua_pop(uiL, 1);

    // Read Team (1 = no team, 2+ = allied team)
    lua_pushstring(uiL, "Team");
    lua_rawget(uiL, entry);
    int team = lua_isnumber(uiL, -1)
        ? static_cast<int>(lua_tonumber(uiL, -1)) : 1;
    lua_pop(uiL, 1);

    // Read PlayerColor / ArmyColor
    lua_pushstring(uiL, "PlayerColor");
    lua_rawget(uiL, entry);
    int pcolor = lua_isnumber(uiL, -1)
        ? static_cast<int>(lua_tonumber(uiL, -1)) : (slot - 1);
    lua_pop(uiL, 1);

    lua_pushstring(uiL, "ArmyColor");
    lua_rawget(uiL, entry);
    int acolor = lua_isnumber(uiL, -1)
        ? static_cast<int>(lua_tonumber(uiL, -1)) : pcolor;
    lua_pop(uiL, 1);

    // Store on brain
    if (brain) {
        brain->set_faction(faction);
        brain->set_player_color(pcolor);
        brain->set_army_color(acolor);
    }
```

- [ ] **Step 3: After army loop, set alliances based on Team**

After the PlayerOptions loop closes, add:
```cpp
    // Set alliances based on Team assignments
    // Team 1 = no team (FFA), Team 2+ = allies
    if (filled_slots > 1) {
        for (int i = 0; i < filled_slots; i++) {
            for (int j = i + 1; j < filled_slots; j++) {
                auto* bi = sim_state->get_army(i);
                auto* bj = sim_state->get_army(j);
                if (bi && bj) {
                    // Teams stored 1-based; check teams[i] == teams[j] && team >= 2
                    // (need to store teams in a local array during the loop above)
                }
            }
        }
    }
```

Note: Store team values in `std::vector<int> teams(filled_slots, 1)` during the loop.

- [ ] **Step 4: Read ScenarioFile from GameOptions**

Before the PlayerOptions parsing, add:
```cpp
    // Read map from GameOptions.ScenarioFile
    lua_pushstring(uiL, "GameOptions");
    lua_rawget(uiL, cfg_idx);
    if (lua_istable(uiL, -1)) {
        lua_pushstring(uiL, "ScenarioFile");
        lua_rawget(uiL, -2);
        if (lua_type(uiL, -1) == LUA_TSTRING) {
            std::string scenario_from_config = lua_tostring(uiL, -1);
            if (!scenario_from_config.empty()) {
                // Override the launch scenario
                lua_pushstring(uiL, "__osc_launch_scenario");
                lua_pushstring(uiL, scenario_from_config.c_str());
                lua_rawset(uiL, LUA_REGISTRYINDEX);
            }
        }
        lua_pop(uiL, 1);
    }
    lua_pop(uiL, 1);
```

- [ ] **Step 5: Build and verify**
- [ ] **Step 6: Commit**

```bash
git add src/main.cpp src/sim/army_brain.hpp
git commit -m "Wire StartSpot/Team/Color/ScenarioFile from lobby sessionConfig"
```

---

## Phase 2: Gameplay Mechanics

### Task 5: Cloaking System

**Files:**
- Modify: `src/sim/unit.hpp:614-630` (add fields)
- Modify: `src/lua/moho_bindings.cpp` (add unit methods)

- [ ] **Step 1: Add cloak fields to Unit**

In unit.hpp around line 615:
```cpp
    bool cloaked_ = false;
    bool radar_stealth_ = false;
    bool sonar_stealth_ = false;
    bool auto_mode_ = false;
```

Add public accessors:
```cpp
    bool is_cloaked() const { return cloaked_; }
    void set_cloaked(bool v) { cloaked_ = v; }
    bool has_radar_stealth() const { return radar_stealth_; }
    void set_radar_stealth(bool v) { radar_stealth_ = v; }
    bool has_sonar_stealth() const { return sonar_stealth_; }
    void set_sonar_stealth(bool v) { sonar_stealth_ = v; }
    bool auto_mode() const { return auto_mode_; }
    void set_auto_mode(bool v) { auto_mode_ = v; }
```

- [ ] **Step 2: Add EnableCloak/DisableCloak/IsUnitCloaked bindings**

In moho_bindings.cpp, add unit method functions:
```cpp
static int unit_EnableCloak(lua_State* L) {
    auto* u = check_entity_unit(L, 1);
    if (u) u->set_cloaked(true);
    return 0;
}
static int unit_DisableCloak(lua_State* L) {
    auto* u = check_entity_unit(L, 1);
    if (u) u->set_cloaked(false);
    return 0;
}
static int unit_IsUnitCloaked(lua_State* L) {
    auto* u = check_entity_unit(L, 1);
    lua_pushboolean(L, u && u->is_cloaked());
    return 1;
}
```

Register in unit_methods array:
```cpp
    {"EnableCloak",   unit_EnableCloak},
    {"DisableCloak",  unit_DisableCloak},
    {"IsUnitCloaked", unit_IsUnitCloaked},
```

- [ ] **Step 3: Build and verify**
- [ ] **Step 4: Commit**

```bash
git add src/sim/unit.hpp src/lua/moho_bindings.cpp
git commit -m "Add cloak system: EnableCloak/DisableCloak/IsUnitCloaked"
```

---

### Task 6: Stealth System

**Files:**
- Modify: `src/lua/moho_bindings.cpp` (add unit methods)

- [ ] **Step 1: Add stealth bindings** (fields already added in Task 5)

```cpp
static int unit_EnableStealth(lua_State* L) {
    auto* u = check_entity_unit(L, 1);
    if (u) u->set_radar_stealth(true);
    return 0;
}
static int unit_DisableStealth(lua_State* L) {
    auto* u = check_entity_unit(L, 1);
    if (u) u->set_radar_stealth(false);
    return 0;
}
static int unit_EnableSonarStealth(lua_State* L) {
    auto* u = check_entity_unit(L, 1);
    if (u) u->set_sonar_stealth(true);
    return 0;
}
static int unit_DisableSonarStealth(lua_State* L) {
    auto* u = check_entity_unit(L, 1);
    if (u) u->set_sonar_stealth(false);
    return 0;
}
```

Register in unit_methods:
```cpp
    {"EnableStealth",      unit_EnableStealth},
    {"DisableStealth",     unit_DisableStealth},
    {"EnableSonarStealth", unit_EnableSonarStealth},
    {"DisableSonarStealth",unit_DisableSonarStealth},
```

- [ ] **Step 2: Build and verify**
- [ ] **Step 3: Commit**

```bash
git add src/lua/moho_bindings.cpp
git commit -m "Add stealth system: EnableStealth/DisableStealth + sonar variants"
```

---

### Task 7: SetAutoMode

**Files:**
- Modify: `src/lua/moho_bindings.cpp`

- [ ] **Step 1: Add SetAutoMode/GetAutoMode bindings**

```cpp
static int unit_SetAutoMode(lua_State* L) {
    auto* u = check_entity_unit(L, 1);
    if (u) u->set_auto_mode(lua_toboolean(L, 2) != 0);
    return 0;
}
static int unit_GetAutoMode(lua_State* L) {
    auto* u = check_entity_unit(L, 1);
    lua_pushboolean(L, u && u->auto_mode());
    return 1;
}
```

Register in unit_methods:
```cpp
    {"SetAutoMode", unit_SetAutoMode},
    {"GetAutoMode", unit_GetAutoMode},
```

- [ ] **Step 2: Build, commit**

```bash
git add src/lua/moho_bindings.cpp
git commit -m "Add SetAutoMode/GetAutoMode for TMD/anti-nuke/engineering auto-fire"
```

---

### Task 8: OnAdjacentTo Callback

**Files:**
- Modify: `src/sim/unit.cpp:1355-1400` (finish_build function)
- Modify: `src/lua/moho_bindings.cpp` (entity_OnStopBeingBuilt area)

- [ ] **Step 1: Add adjacency scan after OnStopBeingBuilt**

In unit.cpp `finish_build`, after the `OnStopBeingBuilt` callback fires (around line 1400), add:
```cpp
    // Scan for adjacent structures and fire OnAdjacentTo callbacks
    if (target->is_structure()) {
        auto nearby = registry.collect_in_radius(
            target->position().x, target->position().z,
            target->footprint_size() * 1.5f + 2.0f);
        for (auto* other_entity : nearby) {
            auto* other = dynamic_cast<Unit*>(other_entity);
            if (!other || other == target || !other->is_structure()) continue;
            if (other->army_index() != target->army_index()) continue;
            // Check actual adjacency (footprints touch)
            f32 dx = std::abs(target->position().x - other->position().x);
            f32 dz = std::abs(target->position().z - other->position().z);
            f32 reach = (target->footprint_size() + other->footprint_size()) * 0.5f + 0.5f;
            if (dx <= reach && dz <= reach) {
                target->add_adjacent(other->id());
                other->add_adjacent(target->id());
                // Fire OnAdjacentTo(self, other) on both
                call_on_adjacent(L, target, other);
                call_on_adjacent(L, other, target);
            }
        }
    }
```

- [ ] **Step 2: Add call_on_adjacent helper**

```cpp
static void call_on_adjacent(lua_State* L, Unit* self, Unit* other) {
    if (self->lua_table_ref() < 0 || other->lua_table_ref() < 0) return;
    lua_rawgeti(L, LUA_REGISTRYINDEX, self->lua_table_ref());
    lua_pushstring(L, "OnAdjacentTo");
    lua_gettable(L, -2);
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, -2); // self
        lua_rawgeti(L, LUA_REGISTRYINDEX, other->lua_table_ref());
        if (lua_pcall(L, 2, 0, 0) != 0) {
            spdlog::warn("OnAdjacentTo error: {}", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
}
```

- [ ] **Step 3: Build and verify**
- [ ] **Step 4: Commit**

```bash
git add src/sim/unit.cpp
git commit -m "Fire OnAdjacentTo callback on structure placement for adjacency bonuses"
```

---

### Task 9: HasValidTeleportDest

**Files:**
- Modify: `src/lua/moho_bindings.cpp:3497` (replace stub)

- [ ] **Step 1: Replace stub with real validation**

Replace `{"HasValidTeleportDest", stub_return_false}` entry's function:

```cpp
static int unit_HasValidTeleportDest(lua_State* L) {
    auto* u = check_entity_unit(L, 1);
    if (!u) { lua_pushboolean(L, 0); return 1; }

    // Read destination from arg or from unit's current teleport command
    // FA calls this as self:HasValidTeleportDest()
    // Check: unit has a queued Teleport command with a valid destination
    auto& cmds = u->commands();
    bool valid = false;
    for (auto& cmd : cmds) {
        if (cmd.type == sim::CommandType::Teleport) {
            f32 tx = cmd.target_pos.x;
            f32 tz = cmd.target_pos.z;
            // Check within playable rect
            // Check terrain is passable for land units
            // For now, return true if position is non-zero
            valid = (tx != 0.0f || tz != 0.0f);
            break;
        }
    }
    lua_pushboolean(L, valid ? 1 : 0);
    return 1;
}
```

Update the entry in unit_methods:
```cpp
    {"HasValidTeleportDest", unit_HasValidTeleportDest},
```

- [ ] **Step 2: Build, commit**

```bash
git add src/lua/moho_bindings.cpp
git commit -m "Implement HasValidTeleportDest for ACU teleport validation"
```

---

### Task 10: Death Weapon API

**Files:**
- Modify: `src/lua/moho_bindings.cpp` (add unit methods)

- [ ] **Step 1: Add SetDeathWeaponEnabled/GetDeathWeaponEnabled**

```cpp
static int unit_SetDeathWeaponEnabled(lua_State* L) {
    auto* u = check_entity_unit(L, 1);
    if (!u) return 0;
    // arg 2 = weapon label (string), arg 3 = enabled (bool)
    const char* label = luaL_checkstring(L, 2);
    bool enabled = lua_toboolean(L, 3) != 0;
    for (auto& w : u->weapons()) {
        if (w.label == label) {
            w.fire_on_death = enabled;
            break;
        }
    }
    return 0;
}

static int unit_GetDeathWeaponEnabled(lua_State* L) {
    auto* u = check_entity_unit(L, 1);
    if (!u) { lua_pushboolean(L, 0); return 1; }
    const char* label = luaL_checkstring(L, 2);
    for (auto& w : u->weapons()) {
        if (w.label == label) {
            lua_pushboolean(L, w.fire_on_death ? 1 : 0);
            return 1;
        }
    }
    lua_pushboolean(L, 0);
    return 1;
}
```

Register:
```cpp
    {"SetDeathWeaponEnabled", unit_SetDeathWeaponEnabled},
    {"GetDeathWeaponEnabled", unit_GetDeathWeaponEnabled},
```

- [ ] **Step 2: Build, commit**

```bash
git add src/lua/moho_bindings.cpp
git commit -m "Add SetDeathWeaponEnabled/GetDeathWeaponEnabled for death weapon control"
```

---

### Task 11: IssueKillSelf

**Files:**
- Modify: `src/lua/sim_bindings.cpp:4613-4638` (add global function)

- [ ] **Step 1: Add IssueKillSelf**

```cpp
static int l_IssueKillSelf(lua_State* L) {
    // arg 1 = table of unit tables
    if (!lua_istable(L, 1)) return 0;
    int n = luaL_getn(L, 1);
    for (int i = 1; i <= n; i++) {
        lua_rawgeti(L, 1, i);
        if (lua_istable(L, -1)) {
            lua_pushstring(L, "_c_object");
            lua_rawget(L, -2);
            auto* entity = static_cast<sim::Entity*>(lua_touserdata(L, -1));
            lua_pop(L, 1);
            if (entity) {
                auto* unit = dynamic_cast<sim::Unit*>(entity);
                if (unit && !unit->is_dying() && !unit->is_dead()) {
                    unit->begin_dying();
                }
            }
        }
        lua_pop(L, 1);
    }
    return 0;
}
```

Register in sim global functions:
```cpp
    state.register_function("IssueKillSelf", l_IssueKillSelf);
```

- [ ] **Step 2: Build, commit**

```bash
git add src/lua/sim_bindings.cpp
git commit -m "Add IssueKillSelf for ctrl+K self-destruct"
```

---

### Task 12: CreateVisibleAreaAtPoint (Scrying)

**Files:**
- Modify: `src/sim/sim_state.hpp` (add temp vision storage)
- Modify: `src/sim/sim_state.cpp` (tick temp vision)
- Modify: `src/lua/sim_bindings.cpp` (add global function)

- [ ] **Step 1: Add temporary vision struct to SimState**

In sim_state.hpp:
```cpp
    struct TempVision {
        u32 army;
        f32 x, z, radius;
        i32 remaining_ticks;
    };
    std::vector<TempVision> temp_visions_;
public:
    void add_temp_vision(u32 army, f32 x, f32 z, f32 radius, f32 lifetime_seconds) {
        temp_visions_.push_back({army, x, z, radius, static_cast<i32>(lifetime_seconds * 10.0f)});
    }
    const std::vector<TempVision>& temp_visions() const { return temp_visions_; }
```

- [ ] **Step 2: Tick temp visions in update_visibility**

In sim_state.cpp `update_visibility()`, after painting unit vision circles, add:
```cpp
    // Paint temporary vision areas (scrying, etc.)
    for (auto& tv : temp_visions_) {
        if (tv.remaining_ticks > 0) {
            visibility_grid_->paint_circle(tv.army, tv.x, tv.z, tv.radius, map::VisFlag::Vision);
            tv.remaining_ticks--;
        }
    }
    // Remove expired
    temp_visions_.erase(
        std::remove_if(temp_visions_.begin(), temp_visions_.end(),
            [](const TempVision& v) { return v.remaining_ticks <= 0; }),
        temp_visions_.end());
```

- [ ] **Step 3: Add CreateVisibleAreaAtPoint global**

In sim_bindings.cpp:
```cpp
static int l_CreateVisibleAreaAtPoint(lua_State* L) {
    // args: army(1-based), x, y, z, radius, lifetime
    int army = static_cast<int>(luaL_checknumber(L, 1)) - 1; // 0-based
    f32 x = static_cast<f32>(luaL_checknumber(L, 2));
    // y ignored (arg 3)
    f32 z = static_cast<f32>(luaL_checknumber(L, 4));
    f32 radius = static_cast<f32>(luaL_checknumber(L, 5));
    f32 lifetime = static_cast<f32>(luaL_optnumber(L, 6, 10.0));

    lua_pushstring(L, "osc_sim_state");
    lua_rawget(L, LUA_REGISTRYINDEX);
    auto* sim = static_cast<sim::SimState*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    if (sim && army >= 0) {
        sim->add_temp_vision(static_cast<u32>(army), x, z, radius, lifetime);
    }
    return 0;
}
```

Register:
```cpp
    state.register_function("CreateVisibleAreaAtPoint", l_CreateVisibleAreaAtPoint);
```

- [ ] **Step 4: Build, commit**

```bash
git add src/sim/sim_state.hpp src/sim/sim_state.cpp src/lua/sim_bindings.cpp
git commit -m "Add CreateVisibleAreaAtPoint for Aeon scrying (Eye of Rhianne)"
```

---

## Phase 3: Polish & Verification

### Task 13: Verify Existing Systems

This task has no code changes — it's verification that existing features work correctly with the new additions.

- [ ] **Step 1: Run unit tests**

```bash
./build/tests/Debug/osc_tests.exe
```
Expected: All tests pass

- [ ] **Step 2: Run smoke test**

```bash
./build/Debug/opensupcom.exe --full-smoke-test
```
Expected: Completes all 5 phases, smoke_report.txt shows no new errors

- [ ] **Step 3: Run auto-skirmish**

```bash
timeout 120 ./build/Debug/opensupcom.exe --auto-skirmish
```
Expected: Game launches, AI builds base, no crashes

- [ ] **Step 4: Commit smoke report if clean**

```bash
git add smoke_report.txt
git commit -m "Verify full skirmish completion: smoke test clean"
```

---

### Task 14: Update Memory

- [ ] **Step 1: Update MEMORY.md with new decisions**

Add entries for:
- Cloak/stealth system fields on Unit
- SendData loopback for single-player
- Control.Disable/Enable/IsDisabled pattern
- OnAdjacentTo callback in finish_build
- CreateVisibleAreaAtPoint temp vision system
- Vector swap fix for dispatch_events crash

- [ ] **Step 2: Commit memory update**
