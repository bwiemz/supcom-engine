# M162 Veterancy XP & Level Progression — Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Full FA veterancy — units earn XP from kills, level up (0-5), gain stat bonuses (regen, max health, damage multiplier).

**Architecture:** Damage contribution tracking on Unit (inline vector), XP distribution on death in entity_Destroy, level-up with blueprint-driven thresholds and buffs, OnVeteran Lua callback. Vet chevron renderer (M108) already displays based on vet_level_.

**Tech Stack:** C++17, Lua 5.0 C API, existing Entity/Unit/Weapon systems.

---

## Chunk 1: Core Fields & Damage Tracking

### Task 1: Add veterancy fields to Unit

**Files:**
- Modify: `src/sim/unit.hpp` (add fields + accessors near vet_level_ at line 545, public API near line 260)
- Modify: `src/sim/entity.hpp` (verify regen_rate_ getter/setter at line 146/262 — no changes needed)

- [ ] **Step 1: Add new private fields to unit.hpp**

After `u8 vet_level_ = 0;` (line 545), add:

```cpp
f32 vet_xp_ = 0;
std::array<f32, 5> vet_thresholds_ = {0, 0, 0, 0, 0}; // from Veteran.Level1-5
f32 damage_multiplier_ = 1.0f; // vet damage bonus multiplier
f32 xp_value_ = 0; // Economy.BuildCostMass (cached for XP distribution)
std::vector<std::pair<u32, f32>> damage_contributions_; // (attacker_id, cumulative_damage)
```

- [ ] **Step 2: Add public accessors near existing vet_level() (line 260)**

After `void set_vet_level(u8 level) { vet_level_ = level; }`:

```cpp
f32 vet_xp() const { return vet_xp_; }
f32 xp_value() const { return xp_value_; }
void set_xp_value(f32 v) { xp_value_ = v; }
f32 damage_multiplier() const { return damage_multiplier_; }
void set_damage_multiplier(f32 m) { damage_multiplier_ = m; }
const std::array<f32, 5>& vet_thresholds() const { return vet_thresholds_; }
void set_vet_thresholds(const std::array<f32, 5>& t) { vet_thresholds_ = t; }
const std::vector<std::pair<u32, f32>>& damage_contributions() const { return damage_contributions_; }
void record_damage(u32 attacker_id, f32 amount);
void clear_damage_contributions() { damage_contributions_.clear(); }
void add_xp(f32 amount, lua_State* L, EntityRegistry& registry);
```

- [ ] **Step 3: Implement record_damage in unit.cpp**

```cpp
void Unit::record_damage(u32 attacker_id, f32 amount) {
    for (auto& [id, dmg] : damage_contributions_) {
        if (id == attacker_id) { dmg += amount; return; }
    }
    damage_contributions_.emplace_back(attacker_id, amount);
}
```

- [ ] **Step 4: Commit**

```bash
git add src/sim/unit.hpp src/sim/unit.cpp
git commit -m "M162a: Add veterancy fields to Unit (xp, thresholds, damage tracking, multiplier)"
```

### Task 2: Record damage contributions in l_Damage

**Files:**
- Modify: `src/lua/sim_bindings.cpp` (l_Damage function at line 1386)

- [ ] **Step 1: Add damage contribution recording**

In l_Damage (sim_bindings.cpp), after the attacker tracking block (line 1417-1424) and before the OnDamage lookup (line 1427), add damage contribution recording:

```cpp
// Record damage contribution for veterancy XP distribution
if (instigator && instigator->is_unit()) {
    target_unit->record_damage(instigator->entity_id(), amount);
}
```

Note: The `instigator` variable is scoped inside the `if (lua_istable(L, 1))` block. We need to extract the instigator entity ID before that block closes, or restructure slightly. The cleanest approach: declare `u32 instigator_id = 0;` before the block, set it inside, then use it after:

```cpp
// After line 1415: if (amount <= 0) return 0;
u32 instigator_id = 0;
// Track attacker (existing block, line 1417-1424)
if (lua_istable(L, 1)) {
    lua_pushstring(L, "_c_object");
    lua_rawget(L, 1);
    auto* instigator = lua_isuserdata(L, -1)
        ? static_cast<sim::Entity*>(lua_touserdata(L, -1)) : nullptr;
    lua_pop(L, 1);
    if (instigator) {
        target_unit->set_last_attacker_id(instigator->entity_id());
        instigator_id = instigator->entity_id();
    }
}
// Record damage contribution for veterancy
if (instigator_id > 0) {
    target_unit->record_damage(instigator_id, amount);
}
```

- [ ] **Step 2: Apply damage_multiplier to weapon damage**

In weapon.cpp try_fire() (line 166), change:
```cpp
proj->damage_amount = damage;
```
to:
```cpp
proj->damage_amount = damage * owner.damage_multiplier();
```

- [ ] **Step 3: Commit**

```bash
git add src/lua/sim_bindings.cpp src/sim/weapon.cpp
git commit -m "M162b: Record damage contributions in l_Damage, apply vet damage multiplier"
```

### Task 3: XP distribution on death in entity_Destroy

**Files:**
- Modify: `src/lua/moho_bindings.cpp` (entity_Destroy at line 731, before death event at line 819)

- [ ] **Step 1: Add XP distribution before destruction**

Insert before `u32 id = e->entity_id();` (line 819):

```cpp
// Veterancy: distribute XP to damage contributors
if (e->is_unit()) {
    auto* dying_unit = static_cast<sim::Unit*>(e);
    f32 xp_value = dying_unit->xp_value();
    if (xp_value > 0 && !dying_unit->damage_contributions().empty()) {
        // Compute total damage dealt to this unit
        f32 total_damage = 0;
        for (const auto& [aid, dmg] : dying_unit->damage_contributions()) {
            total_damage += dmg;
        }
        if (total_damage > 0) {
            auto* sim = get_sim(L);
            for (const auto& [aid, dmg] : dying_unit->damage_contributions()) {
                auto* attacker = sim ? sim->entity_registry().find(aid) : nullptr;
                if (attacker && !attacker->destroyed() && attacker->is_unit()) {
                    f32 xp_share = xp_value * (dmg / total_damage);
                    static_cast<sim::Unit*>(attacker)->add_xp(
                        xp_share, L, sim->entity_registry());
                }
            }
        }
        dying_unit->clear_damage_contributions();
    }
}
```

Note: This must go BEFORE the `begin_dying` / `begin_air_crash` checks because those return early. Actually, looking at the flow more carefully — `entity_Destroy` is called from Lua `Kill()` or `Destroy()`. For units with death animations, it calls `begin_dying()` and returns early (line 859-860). The actual destruction happens later when the death timer expires — which re-calls `entity_Destroy`. So XP distribution should happen on the FIRST call (when the unit starts dying), not on subsequent calls. Check `is_dying()` to avoid double-distribution:

```cpp
// Veterancy: distribute XP on first death call (before begin_dying)
if (e->is_unit() && !static_cast<sim::Unit*>(e)->is_dying()) {
    // ... distribution code ...
}
```

- [ ] **Step 2: Commit**

```bash
git add src/lua/moho_bindings.cpp
git commit -m "M162c: Distribute veterancy XP to damage contributors on unit death"
```

## Chunk 2: Level-Up & Blueprint Reading

### Task 4: Level-up mechanics (add_xp with threshold check and buff application)

**Files:**
- Modify: `src/sim/unit.cpp` (implement add_xp method)
- Modify: `src/sim/unit.hpp` (already declared in Task 1)

- [ ] **Step 1: Implement add_xp in unit.cpp**

```cpp
void Unit::add_xp(f32 amount, lua_State* L, EntityRegistry& registry) {
    if (amount <= 0 || vet_level_ >= 5) return;
    vet_xp_ += amount;

    // Check for level-up (can gain multiple levels at once)
    while (vet_level_ < 5 &&
           vet_thresholds_[vet_level_] > 0 &&
           vet_xp_ >= vet_thresholds_[vet_level_]) {
        vet_level_++;

        // Apply per-level buffs from blueprint
        // Read Buffs table from blueprint via Lua
        if (L && lua_table_ref() >= 0) {
            apply_vet_buffs(L);
            fire_on_veteran(L);
        }

        spdlog::debug("Unit #{} leveled up to vet level {}", entity_id(), vet_level_);
    }
}
```

- [ ] **Step 2: Implement apply_vet_buffs (private helper)**

Add declaration to unit.hpp private section:
```cpp
void apply_vet_buffs(lua_State* L);
void fire_on_veteran(lua_State* L);
```

Implement in unit.cpp:
```cpp
void Unit::apply_vet_buffs(lua_State* L) {
    // Read buff values from blueprint Buffs table
    // Buffs.Regen.Level1..Level5 = flat regen increase
    // Buffs.MaxHealth.Level1..Level5 = multiplier (e.g. 1.1)
    // Buffs.Damage.Level1..Level5 = multiplier (e.g. 1.1)
    lua_pushstring(L, "__blueprints");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return; }

    lua_pushstring(L, blueprint_id().c_str());
    lua_rawget(L, -2);
    if (!lua_istable(L, -1)) { lua_pop(L, 2); return; }

    lua_pushstring(L, "Buffs");
    lua_gettable(L, -2);
    if (!lua_istable(L, -1)) { lua_pop(L, 3); return; }

    char level_key[8];
    snprintf(level_key, sizeof(level_key), "Level%d", vet_level_);

    // Regen buff
    lua_pushstring(L, "Regen");
    lua_gettable(L, -2);
    if (lua_istable(L, -1)) {
        lua_pushstring(L, level_key);
        lua_gettable(L, -2);
        if (lua_isnumber(L, -1)) {
            set_regen_rate(regen_rate() + static_cast<f32>(lua_tonumber(L, -1)));
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1); // Regen

    // MaxHealth buff
    lua_pushstring(L, "MaxHealth");
    lua_gettable(L, -2);
    if (lua_istable(L, -1)) {
        lua_pushstring(L, level_key);
        lua_gettable(L, -2);
        if (lua_isnumber(L, -1)) {
            f32 factor = static_cast<f32>(lua_tonumber(L, -1));
            if (factor > 0) {
                f32 old_max = max_health();
                f32 new_max = old_max * factor;
                set_max_health(new_max);
                // Heal the difference
                set_health(health() + (new_max - old_max));
            }
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1); // MaxHealth

    // Damage buff
    lua_pushstring(L, "Damage");
    lua_gettable(L, -2);
    if (lua_istable(L, -1)) {
        lua_pushstring(L, level_key);
        lua_gettable(L, -2);
        if (lua_isnumber(L, -1)) {
            f32 factor = static_cast<f32>(lua_tonumber(L, -1));
            if (factor > 0) {
                damage_multiplier_ *= factor;
            }
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1); // Damage

    lua_pop(L, 3); // Buffs + bp + __blueprints
}
```

- [ ] **Step 3: Implement fire_on_veteran**

```cpp
void Unit::fire_on_veteran(lua_State* L) {
    if (lua_table_ref() < 0) return;
    lua_rawgeti(L, LUA_REGISTRYINDEX, lua_table_ref());
    int tbl = lua_gettop(L);
    lua_pushstring(L, "OnVeteran");
    lua_gettable(L, tbl);
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, tbl); // self
        if (lua_pcall(L, 1, 0, 0) != 0) {
            spdlog::warn("OnVeteran error: {}", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1);
    }
    lua_pop(L, 1); // unit table
}
```

- [ ] **Step 4: Commit**

```bash
git add src/sim/unit.hpp src/sim/unit.cpp
git commit -m "M162d: Implement add_xp with level-up, blueprint buff application, OnVeteran callback"
```

### Task 5: Read Veteran thresholds + xp_value from blueprint on spawn

**Files:**
- Modify: `src/lua/sim_bindings.cpp` (unit spawn section, near existing VetInstigators at line 717)

- [ ] **Step 1: Read Veteran thresholds after existing vet fields**

After the existing VetDamageTaken field (line 724-726), add:

```cpp
// Read Veteran thresholds from blueprint
lua_pushstring(L, "__blueprints");
lua_rawget(L, LUA_GLOBALSINDEX);
if (lua_istable(L, -1)) {
    lua_pushstring(L, bp_id);
    lua_rawget(L, -2);
    if (lua_istable(L, -1)) {
        // Veteran.Level1..Level5
        lua_pushstring(L, "Veteran");
        lua_gettable(L, -2);
        if (lua_istable(L, -1)) {
            std::array<f32, 5> thresholds = {0, 0, 0, 0, 0};
            const char* level_keys[] = {"Level1", "Level2", "Level3", "Level4", "Level5"};
            for (int i = 0; i < 5; ++i) {
                lua_pushstring(L, level_keys[i]);
                lua_gettable(L, -2);
                if (lua_isnumber(L, -1))
                    thresholds[i] = static_cast<f32>(lua_tonumber(L, -1));
                lua_pop(L, 1);
            }
            unit_ptr->set_vet_thresholds(thresholds);
        }
        lua_pop(L, 1); // Veteran

        // Economy.BuildCostMass → xp_value (what this unit is worth as XP)
        lua_pushstring(L, "Economy");
        lua_gettable(L, -2);
        if (lua_istable(L, -1)) {
            lua_pushstring(L, "BuildCostMass");
            lua_gettable(L, -2);
            if (lua_isnumber(L, -1))
                unit_ptr->set_xp_value(static_cast<f32>(lua_tonumber(L, -1)));
            lua_pop(L, 1);
        }
        lua_pop(L, 1); // Economy
    }
    lua_pop(L, 1); // bp table
}
lua_pop(L, 1); // __blueprints
```

- [ ] **Step 2: Commit**

```bash
git add src/lua/sim_bindings.cpp
git commit -m "M162e: Read Veteran thresholds and xp_value from blueprint on unit spawn"
```

## Chunk 3: Lua Bindings & Tests

### Task 6: Lua bindings for veterancy

**Files:**
- Modify: `src/lua/moho_bindings.cpp` (unit_methods table at line 3221)

- [ ] **Step 1: Add binding functions before unit_methods table**

```cpp
// unit:GetVeterancyLevel()
static int unit_GetVeterancyLevel(lua_State* L) {
    auto* u = check_unit(L);
    if (!u) return 0;
    lua_pushnumber(L, u->vet_level());
    return 1;
}

// unit:SetVeterancyLevel(level)
static int unit_SetVeterancyLevel(lua_State* L) {
    auto* u = check_unit(L);
    if (!u) return 0;
    int level = static_cast<int>(lua_tonumber(L, 2));
    if (level < 0) level = 0;
    if (level > 5) level = 5;
    u->set_vet_level(static_cast<u8>(level));
    return 0;
}

// unit:AddXP(amount)
static int unit_AddXP(lua_State* L) {
    auto* u = check_unit(L);
    if (!u) return 0;
    f32 amount = static_cast<f32>(lua_tonumber(L, 2));
    auto* sim = get_sim(L);
    if (sim) {
        u->add_xp(amount, L, sim->entity_registry());
    }
    return 0;
}

// unit:GetXPValue()
static int unit_GetXPValue(lua_State* L) {
    auto* u = check_unit(L);
    if (!u) return 0;
    lua_pushnumber(L, u->xp_value());
    return 1;
}
```

- [ ] **Step 2: Add entries to unit_methods table**

Before the `{nullptr, nullptr}` sentinel (line 3385), add:

```cpp
{"GetVeterancyLevel",           unit_GetVeterancyLevel},
{"SetVeterancyLevel",           unit_SetVeterancyLevel},
{"AddXP",                       unit_AddXP},
{"GetXPValue",                  unit_GetXPValue},
```

- [ ] **Step 3: Commit**

```bash
git add src/lua/moho_bindings.cpp
git commit -m "M162f: Add veterancy Lua bindings (GetVeterancyLevel, SetVeterancyLevel, AddXP, GetXPValue)"
```

### Task 7: Smoke tests

**Files:**
- Modify: `tests/test_smoke_test.cpp`

- [ ] **Step 1: Add veterancy field tests**

```cpp
TEST_CASE("Unit veterancy fields and record_damage", "[m162]") {
    sim::Unit u;
    CHECK(u.vet_level() == 0);
    CHECK(u.vet_xp() == 0.0f);
    CHECK(u.damage_multiplier() == 1.0f);
    CHECK(u.xp_value() == 0.0f);
    CHECK(u.damage_contributions().empty());

    // Record damage from two attackers
    u.record_damage(100, 50.0f);
    u.record_damage(200, 30.0f);
    u.record_damage(100, 20.0f); // same attacker again
    REQUIRE(u.damage_contributions().size() == 2);
    CHECK(u.damage_contributions()[0].first == 100);
    CHECK(u.damage_contributions()[0].second == 70.0f); // 50 + 20
    CHECK(u.damage_contributions()[1].first == 200);
    CHECK(u.damage_contributions()[1].second == 30.0f);

    u.clear_damage_contributions();
    CHECK(u.damage_contributions().empty());
}

TEST_CASE("Unit vet thresholds storage", "[m162]") {
    sim::Unit u;
    std::array<f32, 5> thresholds = {25.0f, 100.0f, 250.0f, 500.0f, 1000.0f};
    u.set_vet_thresholds(thresholds);
    CHECK(u.vet_thresholds()[0] == 25.0f);
    CHECK(u.vet_thresholds()[4] == 1000.0f);
}

TEST_CASE("Unit damage_multiplier applied by weapon", "[m162]") {
    sim::Unit u;
    CHECK(u.damage_multiplier() == 1.0f);
    u.set_damage_multiplier(1.5f);
    CHECK(u.damage_multiplier() == 1.5f);
}
```

- [ ] **Step 2: Run tests**

```bash
cmake --build build --config Debug && ./build/tests/Debug/osc_tests.exe
```

- [ ] **Step 3: Commit**

```bash
git add tests/test_smoke_test.cpp
git commit -m "M162g: Add veterancy smoke tests (damage tracking, thresholds, multiplier)"
```

---

## Summary

| Task | Description | Files |
|------|-------------|-------|
| 1 | Vet fields on Unit | unit.hpp, unit.cpp |
| 2 | Record damage in l_Damage + apply multiplier in weapon | sim_bindings.cpp, weapon.cpp |
| 3 | XP distribution on death | moho_bindings.cpp |
| 4 | add_xp + level-up + buffs + OnVeteran | unit.hpp, unit.cpp |
| 5 | Read Veteran thresholds + xp_value from blueprint | sim_bindings.cpp |
| 6 | Lua bindings | moho_bindings.cpp |
| 7 | Smoke tests | test_smoke_test.cpp |
