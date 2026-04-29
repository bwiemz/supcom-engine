# M157-M159: Air Movement Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** FA-accurate air movement — units fly at blueprint elevation, bank on turns, accelerate/decelerate, engage air/ground targets, consume fuel, and crash on death with impact damage.

**Architecture:** Air movement fields added to Unit. Navigator gets an `update_air()` method for heading-based flight (turn→accelerate→advance→altitude→bank). Weapon targeting adds layer filtering. Air death overrides the existing dying system with gravity-driven crash physics. Boids-style separation prevents air unit stacking.

**Tech Stack:** C++17, Vulkan, Lua 5.0, CMake + vcpkg

---

## File Structure

**Modify:**
- `src/sim/unit.hpp` — add air movement fields (heading_, pitch_, bank_angle_, current_airspeed_, current_altitude_, max_airspeed_, turn_rate_rad_, accel_rate_, climb_rate_, elevation_target_, crash state fields)
- `src/sim/unit.cpp` — air separation in update(), crash physics in tick_dying(), fuel consumption
- `src/sim/navigator.hpp` — add `update_air()` method declaration
- `src/sim/navigator.cpp` — implement heading-based air movement
- `src/lua/sim_bindings.cpp` — read Air/Physics blueprint fields in create_unit_core()
- `src/sim/weapon.hpp` — add bomb_drop fields
- `src/sim/weapon.cpp` — add layer filtering in update_targeting(), bomb drop logic
- `src/sim/projectile.cpp` — ballistic bomb projectile (gravity, no tracking)
- `src/lua/moho_bindings.cpp` — wire begin_air_crash() into entity_Destroy
- `src/sim/sim_state.cpp` — crash impact detection and splash damage
- `tests/test_smoke_test.cpp` — air movement, crash, fuel, targeting tests

**No new files needed** — all changes fit naturally into existing modules.

**Deferred scope (follow-up milestones):**
- Air staging platforms (dock/refuel/repair) — requires transport system extension
- Bomber circle-back for second pass — requires attack-move state machine
- Auto-return-to-base on low fuel — requires AI command generation
These are explicitly deferred and do not block the core air movement system.

**Renderer note:** The existing unit renderer reads `entity.orientation()` for model matrix construction via `build_model_matrix()`. Setting orientation via `euler_to_quat(heading, pitch, bank)` in Navigator::update_air() automatically produces correct visual banking/pitching. No renderer changes needed.

---

## Chunk 1: Flight Physics Core (M157)

### Task 1: Add Air Movement Fields to Unit

**Files:**
- Modify: `src/sim/unit.hpp` — add fields + getters/setters
- Test: `tests/test_smoke_test.cpp`

- [ ] **Step 1: Add air movement fields to Unit class**

In `src/sim/unit.hpp`, add public getters/setters and private fields:

```cpp
// Air movement (populated from blueprint Air subtable)
f32 heading() const { return heading_; }
void set_heading(f32 h) { heading_ = h; }
f32 pitch() const { return pitch_; }
void set_pitch(f32 p) { pitch_ = p; }
f32 bank_angle() const { return bank_angle_; }
void set_bank_angle(f32 b) { bank_angle_ = b; }
f32 current_airspeed() const { return current_airspeed_; }
void set_current_airspeed(f32 s) { current_airspeed_ = s; }
f32 current_altitude() const { return current_altitude_; }
void set_current_altitude(f32 a) { current_altitude_ = a; }
f32 max_airspeed() const { return max_airspeed_; }
void set_max_airspeed(f32 s) { max_airspeed_ = s; }
f32 turn_rate_rad() const { return turn_rate_rad_; }
void set_turn_rate_rad(f32 r) { turn_rate_rad_ = r; }
f32 accel_rate() const { return accel_rate_; }
void set_accel_rate(f32 r) { accel_rate_ = r; }
f32 climb_rate() const { return climb_rate_; }
void set_climb_rate(f32 r) { climb_rate_ = r; }
f32 elevation_target() const { return elevation_target_; }
void set_elevation_target(f32 e) { elevation_target_ = e; }
bool is_air_unit() const { return layer_ == "Air"; }
```

Private fields (add after fuel system fields, around line 513):

```cpp
// Air movement state
f32 heading_ = 0;            // yaw in radians
f32 pitch_ = 0;              // pitch in radians (visual only for dive/climb)
f32 bank_angle_ = 0;         // roll in radians (visual banking on turns)
f32 current_airspeed_ = 0;   // current speed (ramps toward max_airspeed_)
f32 current_altitude_ = 0;   // actual Y offset above terrain
f32 max_airspeed_ = 0;       // from blueprint Air.MaxAirspeed (fallback: max_speed_)
f32 turn_rate_rad_ = 0;      // yaw rate rad/s, from Air.TurnSpeed (deg→rad)
f32 accel_rate_ = 0;         // from Air.AccelerateRate (fallback: max_airspeed * 0.5)
f32 climb_rate_ = 5.0f;      // vertical speed limit (units/sec)
f32 elevation_target_ = 18.0f; // target altitude above terrain, from Physics.Elevation
```

- [ ] **Step 2: Add crash state fields to Unit**

Also in `src/sim/unit.hpp`, add crash fields for M159 (declare now, implement later):

```cpp
// Air crash state (M159)
bool is_crashing() const { return crashing_; }
bool crash_impacted() const { return crash_impacted_; }
f32 crash_velocity_y() const { return crash_velocity_y_; }
f32 crash_spin_rate() const { return crash_spin_rate_; }
f32 crash_damage() const { return crash_damage_; }
void set_crash_damage(f32 d) { crash_damage_ = d; }
```

Private fields:

```cpp
// Air crash state
bool crashing_ = false;
bool crash_impacted_ = false; // set once on terrain impact, consumed by SimState
f32 crash_velocity_y_ = 0;
f32 crash_spin_rate_ = 0;
f32 crash_damage_ = 100.0f;  // from blueprint General.CrashDamage
```

- [ ] **Step 3: Write test for air fields**

```cpp
TEST_CASE("Air unit fields initialize correctly", "[m157]") {
    sim::Unit unit;
    unit.set_layer("Air");
    unit.set_max_airspeed(15.0f);
    unit.set_turn_rate_rad(1.5f);
    unit.set_elevation_target(20.0f);
    unit.set_accel_rate(7.5f);

    CHECK(unit.is_air_unit());
    CHECK(unit.max_airspeed() == 15.0f);
    CHECK(unit.turn_rate_rad() == 1.5f);
    CHECK(unit.elevation_target() == 20.0f);
    CHECK(unit.heading() == 0.0f);
    CHECK(unit.current_airspeed() == 0.0f);
    CHECK(unit.current_altitude() == 0.0f);
}
```

- [ ] **Step 4: Build and run test**

Run: `cmake --build build --config Debug --target osc_tests && ./build/tests/Debug/osc_tests.exe "[m157]"`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add src/sim/unit.hpp tests/test_smoke_test.cpp
git commit -m "M157a: Add air movement fields to Unit class"
```

---

### Task 2: Read Air Blueprint Fields in create_unit_core()

**Files:**
- Modify: `src/lua/sim_bindings.cpp` — add Air subtable reads after existing Physics reads

- [ ] **Step 1: Read Physics.Elevation for air units**

In `sim_bindings.cpp`, inside `create_unit_core()`, after the existing `Physics.MotionType` read block (around line 516), add:

```cpp
// Read Physics.Elevation for air units (target flight altitude)
if (unit->is_air_unit()) {
    store->push_lua_table(*entry, L);
    lua_pushstring(L, "Physics");
    lua_rawget(L, -2);
    if (lua_istable(L, -1)) {
        lua_pushstring(L, "Elevation");
        lua_rawget(L, -2);
        if (lua_isnumber(L, -1)) {
            unit->set_elevation_target(static_cast<f32>(lua_tonumber(L, -1)));
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 2); // Physics table + bp table
}
```

- [ ] **Step 2: Read Air subtable fields**

After the Physics.Elevation read, add the Air subtable read:

```cpp
// Read Air subtable for air units
if (unit->is_air_unit()) {
    store->push_lua_table(*entry, L);
    lua_pushstring(L, "Air");
    lua_rawget(L, -2);
    if (lua_istable(L, -1)) {
        // Air.MaxAirspeed
        lua_pushstring(L, "MaxAirspeed");
        lua_rawget(L, -2);
        if (lua_isnumber(L, -1))
            unit->set_max_airspeed(static_cast<f32>(lua_tonumber(L, -1)));
        lua_pop(L, 1);

        // Air.TurnSpeed (degrees → radians)
        lua_pushstring(L, "TurnSpeed");
        lua_rawget(L, -2);
        if (lua_isnumber(L, -1)) {
            f32 deg = static_cast<f32>(lua_tonumber(L, -1));
            unit->set_turn_rate_rad(deg * 3.14159265f / 180.0f);
        }
        lua_pop(L, 1);

        // Air.AccelerateRate
        lua_pushstring(L, "AccelerateRate");
        lua_rawget(L, -2);
        if (lua_isnumber(L, -1))
            unit->set_accel_rate(static_cast<f32>(lua_tonumber(L, -1)));
        lua_pop(L, 1);
    }
    lua_pop(L, 2); // Air table + bp table

    // Fallbacks: if Air subtable was missing or incomplete
    if (unit->max_airspeed() <= 0 && unit->max_speed() > 0)
        unit->set_max_airspeed(unit->max_speed());
    if (unit->accel_rate() <= 0 && unit->max_airspeed() > 0)
        unit->set_accel_rate(unit->max_airspeed() * 0.5f);
    if (unit->turn_rate_rad() <= 0)
        unit->set_turn_rate_rad(1.5f); // ~86 deg/s default
}
```

- [ ] **Step 3: Read General.CrashDamage**

After the Air subtable read:

```cpp
// Read General.CrashDamage for air units
if (unit->is_air_unit()) {
    store->push_lua_table(*entry, L);
    lua_pushstring(L, "General");
    lua_rawget(L, -2);
    if (lua_istable(L, -1)) {
        lua_pushstring(L, "CrashDamage");
        lua_rawget(L, -2);
        if (lua_isnumber(L, -1))
            unit->set_crash_damage(static_cast<f32>(lua_tonumber(L, -1)));
        lua_pop(L, 1);
    }
    lua_pop(L, 2);
}
```

- [ ] **Step 4: Build and verify**

Run: `cmake --build build --config Debug`
Expected: Clean build

- [ ] **Step 5: Commit**

```bash
git add src/lua/sim_bindings.cpp
git commit -m "M157b: Read Air blueprint fields (MaxAirspeed, TurnSpeed, Elevation, CrashDamage)"
```

---

### Task 3: Implement Navigator::update_air() — Heading-Based Flight

**Files:**
- Modify: `src/sim/navigator.hpp` — add update_air() declaration
- Modify: `src/sim/navigator.cpp` — implement heading-based air movement

- [ ] **Step 1: Add update_air() declaration to Navigator**

In `navigator.hpp`, add after the existing `update()` declaration:

```cpp
/// Air-specific movement: heading-based steering, acceleration, altitude management.
/// Reads/writes air state on the Unit. Returns true if still moving.
bool update_air(class Unit& unit, f64 dt,
                const map::Terrain* terrain = nullptr);
```

Add `#include "sim/unit.hpp"` or forward declare Unit — use forward declaration since navigator.hpp is included by unit.hpp. Actually, unit.hpp already includes navigator.hpp, so we can't include unit.hpp from navigator.hpp (circular). Use a forward declaration:

The existing forward declaration `class SimState;` is already there. Add `class Unit;` next to it.

- [ ] **Step 2: Implement update_air()**

In `navigator.cpp`, add:

```cpp
#include "sim/unit.hpp"
```

Then implement:

```cpp
bool Navigator::update_air(Unit& unit, f64 dt,
                            const map::Terrain* terrain) {
    if (status_ == Status::Idle) return false;
    if (waypoints_.empty() || waypoint_index_ >= waypoints_.size()) {
        status_ = Status::Idle;
        return false;
    }

    f32 fdt = static_cast<f32>(dt);
    auto pos = unit.position();

    // Current waypoint target
    const auto& wp = waypoints_[waypoint_index_];
    bool is_final = (waypoint_index_ == waypoints_.size() - 1);

    // --- 1. Heading: turn toward target ---
    f32 dx = wp.x - pos.x;
    f32 dz = wp.z - pos.z;
    f32 desired_heading = std::atan2(dx, dz); // atan2(x,z) for Y-up heading
    f32 heading = unit.heading();

    // Shortest-arc angle difference
    f32 angle_diff = desired_heading - heading;
    // Normalize to [-PI, PI]
    while (angle_diff > 3.14159265f) angle_diff -= 6.28318530f;
    while (angle_diff < -3.14159265f) angle_diff += 6.28318530f;

    f32 max_turn = unit.turn_rate_rad() * unit.turn_mult() * fdt;
    f32 actual_turn = 0;
    if (std::abs(angle_diff) <= max_turn) {
        heading = desired_heading;
        actual_turn = angle_diff;
    } else {
        f32 sign = (angle_diff > 0) ? 1.0f : -1.0f;
        heading += sign * max_turn;
        actual_turn = sign * max_turn;
    }
    // Normalize heading to [0, 2PI]
    while (heading < 0) heading += 6.28318530f;
    while (heading >= 6.28318530f) heading -= 6.28318530f;
    unit.set_heading(heading);

    // --- 2. Banking: proportional to turn rate ---
    f32 bank = std::clamp(actual_turn / fdt * 0.5f, -0.5f, 0.5f);
    // Smooth bank (lerp toward target bank)
    f32 cur_bank = unit.bank_angle();
    cur_bank += (bank - cur_bank) * std::min(1.0f, 5.0f * fdt);
    unit.set_bank_angle(cur_bank);

    // --- 3. Acceleration ---
    f32 airspeed = unit.current_airspeed();
    f32 target_speed = unit.max_airspeed() * unit.speed_mult();
    f32 accel = unit.accel_rate() * unit.accel_mult();
    if (airspeed < target_speed) {
        airspeed = std::min(airspeed + accel * fdt, target_speed);
    } else if (airspeed > target_speed) {
        airspeed = std::max(airspeed - accel * fdt, target_speed);
    }
    unit.set_current_airspeed(airspeed);

    // --- 4. Move along heading ---
    f32 step = airspeed * fdt;
    pos.x += std::sin(heading) * step;
    pos.z += std::cos(heading) * step;

    // --- 5. Altitude management ---
    // Use get_terrain_height (NOT get_surface_height) — air units fly above terrain,
    // not above water surface. Ground navigator uses get_surface_height instead.
    f32 terrain_h = terrain ? terrain->get_terrain_height(pos.x, pos.z) : 0;
    f32 target_alt = unit.elevation_target();
    f32 target_y = terrain_h + target_alt;
    f32 alt = unit.current_altitude();
    f32 climb = unit.climb_rate() * fdt;
    if (alt < target_alt) {
        alt = std::min(alt + climb, target_alt);
    } else if (alt > target_alt) {
        alt = std::max(alt - climb, target_alt);
    }
    unit.set_current_altitude(alt);
    pos.y = terrain_h + alt;

    // --- 6. Pitch: visual dive/climb indication ---
    f32 pitch = (target_y - pos.y) * 0.02f; // gentle pitch proportional to altitude error
    pitch = std::clamp(pitch, -0.3f, 0.3f);
    unit.set_pitch(pitch);

    // --- 7. Set orientation from euler angles ---
    unit.set_orientation(euler_to_quat(heading, pitch, cur_bank));

    // --- 8. Clamp to playable area ---
    if (sim_) pos = sim_->clamp_to_playable(pos);
    unit.set_position(pos);

    // --- 9. Check waypoint arrival (2D distance) ---
    f32 dist2 = (wp.x - pos.x) * (wp.x - pos.x) + (wp.z - pos.z) * (wp.z - pos.z);
    f32 tolerance = is_final ? ARRIVAL_TOLERANCE : WAYPOINT_TOLERANCE;
    // Air units use larger arrival tolerance since they can't stop on a dime
    f32 air_tolerance = std::max(tolerance, airspeed * 0.5f);
    if (dist2 <= air_tolerance * air_tolerance) {
        if (is_final) {
            if (!speed_through_goal_) {
                status_ = Status::Idle;
                waypoints_.clear();
                waypoint_index_ = 0;
                return false;
            }
            // Speed through: remain moving but mark arrival
            status_ = Status::Idle;
            return false;
        }
        waypoint_index_++;
    }

    return true;
}
```

- [ ] **Step 3: Wire up air movement in Unit::update()**

In `src/sim/unit.cpp`, in `Unit::update()`, find every call to `navigator_.update(*this, effective_speed(), dt, ctx.terrain)` and wrap with a layer check. For the Move command case (around line 238):

Replace:
```cpp
if (!navigator_.update(*this, effective_speed(), dt, ctx.terrain)) {
```
With:
```cpp
bool still_moving = is_air_unit()
    ? navigator_.update_air(*this, dt, ctx.terrain)
    : navigator_.update(*this, effective_speed(), dt, ctx.terrain);
if (!still_moving) {
```

Apply the same pattern to the primary movement commands that air units actually use:
- Move command (~line 238) — primary air movement
- Attack command (~line 277) — air-to-air/ground engagement
- Patrol command (~line 338) — air patrol routes

Other commands (BuildMobile, Reclaim, Repair, Guard) are ground-only operations — air units don't use them, so leave those as-is with `navigator_.update()`. Add TODO comments for future air-specific handling if needed.

To keep the three cases DRY, add a private helper method to Unit:

```cpp
// In unit.hpp, private:
bool nav_update(f64 dt, const map::Terrain* terrain);
```

```cpp
// In unit.cpp:
bool Unit::nav_update(f64 dt, const map::Terrain* terrain) {
    if (is_air_unit())
        return navigator_.update_air(*this, dt, terrain);
    return navigator_.update(*this, effective_speed(), dt, terrain);
}
```

Then replace `navigator_.update(*this, effective_speed(), dt, ctx.terrain)` with `nav_update(dt, ctx.terrain)` in the Move, Attack, and Patrol cases.

- [ ] **Step 4: Write test for air movement**

```cpp
TEST_CASE("Navigator::update_air moves unit along heading", "[m157]") {
    sim::Unit unit;
    unit.set_layer("Air");
    unit.set_max_airspeed(10.0f);
    unit.set_turn_rate_rad(3.14f); // fast turn for test
    unit.set_accel_rate(100.0f);   // instant accel for test
    unit.set_elevation_target(20.0f);
    unit.set_climb_rate(100.0f);   // fast climb for test
    unit.set_position({0, 0, 0});

    auto& nav = unit.navigator();
    nav.set_goal({100, 0, 100}); // straight-line goal

    // Run several ticks
    for (int i = 0; i < 20; i++) {
        nav.update_air(unit, 0.1, nullptr);
    }

    // Should have moved toward goal
    CHECK(unit.position().x > 0);
    CHECK(unit.position().z > 0);
    // Should have gained altitude
    CHECK(unit.current_altitude() > 0);
    // Should have nonzero airspeed
    CHECK(unit.current_airspeed() > 0);
}
```

- [ ] **Step 5: Build and run tests**

Run: `cmake --build build --config Debug --target osc_tests && ./build/tests/Debug/osc_tests.exe "[m157]"`
Expected: PASS

- [ ] **Step 6: Commit**

```bash
git add src/sim/navigator.hpp src/sim/navigator.cpp src/sim/unit.hpp src/sim/unit.cpp tests/test_smoke_test.cpp
git commit -m "M157c: Implement heading-based air movement in Navigator::update_air()"
```

---

### Task 4: Air Unit Separation (Boids Repulsion)

**Files:**
- Modify: `src/sim/unit.cpp` — add air separation in update()

- [ ] **Step 1: Add air separation after movement in Unit::update()**

In `src/sim/unit.cpp`, after the `done_commands:` label (after command processing but before weapons), add air separation logic:

```cpp
// Air unit separation: lightweight boids repulsion to prevent stacking
if (is_air_unit() && !dying_ && navigator_.is_moving()) {
    constexpr f32 SEPARATION_RADIUS = 8.0f;
    constexpr f32 SEPARATION_FORCE = 3.0f;
    auto nearby = ctx.registry.collect_in_radius(position().x, position().z, SEPARATION_RADIUS);
    f32 repulse_x = 0, repulse_z = 0;
    for (u32 nid : nearby) {
        if (nid == entity_id()) continue;
        auto* ne = ctx.registry.find(nid);
        if (!ne || ne->destroyed() || !ne->is_unit()) continue;
        auto* nu = static_cast<Unit*>(ne);
        if (!nu->is_air_unit() || nu->army() != army()) continue;
        f32 ndx = position().x - ne->position().x;
        f32 ndz = position().z - ne->position().z;
        f32 nd2 = ndx * ndx + ndz * ndz;
        if (nd2 > 0.01f && nd2 < SEPARATION_RADIUS * SEPARATION_RADIUS) {
            f32 inv = 1.0f / std::sqrt(nd2);
            repulse_x += ndx * inv;
            repulse_z += ndz * inv;
        }
    }
    if (repulse_x != 0 || repulse_z != 0) {
        auto p = position();
        f32 fdt = static_cast<f32>(dt);
        p.x += repulse_x * SEPARATION_FORCE * fdt;
        p.z += repulse_z * SEPARATION_FORCE * fdt;
        set_position(p);
    }
}
```

- [ ] **Step 2: Build and verify**

Run: `cmake --build build --config Debug`
Expected: Clean build

- [ ] **Step 3: Commit**

```bash
git add src/sim/unit.cpp
git commit -m "M157d: Add boids-style air unit separation to prevent stacking"
```

---

### Task 5: Air Unit Initialization — Set Initial Altitude and Heading

**Files:**
- Modify: `src/lua/sim_bindings.cpp` — set initial altitude when air unit spawns

- [ ] **Step 1: Initialize air unit altitude at creation**

In `create_unit_core()`, after all blueprint reads and position setup, for air units set their initial altitude and Y position:

```cpp
// Air units: start at flight altitude, face a default heading
if (unit->is_air_unit()) {
    unit->set_current_altitude(unit->elevation_target());
    // Set Y position above terrain
    f32 terrain_h = 0;
    if (sim && sim->terrain())
        terrain_h = sim->terrain()->get_terrain_height(unit->position().x, unit->position().z);
    auto p = unit->position();
    p.y = terrain_h + unit->elevation_target();
    unit->set_position(p);
    // Initialize heading from unit's current orientation (or 0)
    unit->set_heading(0);
    unit->set_orientation(euler_to_quat(0, 0, 0));
}
```

- [ ] **Step 2: Build and verify**

Run: `cmake --build build --config Debug`
Expected: Clean build

- [ ] **Step 3: Commit**

```bash
git add src/lua/sim_bindings.cpp
git commit -m "M157e: Initialize air unit altitude and heading at creation"
```

---

## Chunk 2: Air Combat & Fuel (M158)

### Task 6: Weapon Layer Targeting Filter

**Files:**
- Modify: `src/sim/weapon.cpp` — filter targets by layer compatibility in update_targeting()

**Context:** Weapon already has `fire_target_layer_caps` bitmask and `layer_to_bit()` / `parse_layer_caps()` helpers in `weapon.hpp`. The `update_targeting()` method scans for targets but does NOT currently filter by layer compatibility.

- [ ] **Step 1: Add layer check to weapon target acquisition**

In `weapon.cpp`, find `update_targeting()` (or equivalent target scanning function). When iterating candidate targets, add:

```cpp
// Filter by layer compatibility
if (candidate->is_unit()) {
    auto* cu = static_cast<const Unit*>(candidate);
    uint8_t target_bit = layer_to_bit(cu->layer());
    if ((fire_target_layer_caps & target_bit) == 0)
        continue; // This weapon cannot target this layer
}
```

- [ ] **Step 2: Read weapon RangeCategory from blueprint**

In `sim_bindings.cpp` where weapons are created from blueprint data, read `RangeCategory` and convert to `fire_target_layer_caps`:

```cpp
// Use lua_gettable to match existing weapon blueprint read style
lua_pushstring(L, "RangeCategory");
lua_gettable(L, -2);
if (lua_isstring(L, -1)) {
    std::string rc = lua_tostring(L, -1);
    if (rc == "UWRC_AntiAir")
        weapon->fire_target_layer_caps = layer_to_bit("Air");
    else if (rc == "UWRC_DirectFire")
        weapon->fire_target_layer_caps = layer_to_bit("Land") | layer_to_bit("Water") | layer_to_bit("Seabed");
    else if (rc == "UWRC_AntiNavy")
        weapon->fire_target_layer_caps = layer_to_bit("Water") | layer_to_bit("Sub") | layer_to_bit("Seabed");
    else if (rc == "UWRC_Countermeasure")
        weapon->fire_target_layer_caps = 0xFF; // all layers
    // Default (no RangeCategory): 0xFF = all layers
}
lua_pop(L, 1);
```

- [ ] **Step 3: Write test**

```cpp
TEST_CASE("Weapon layer targeting filters correctly", "[m158]") {
    CHECK(sim::layer_to_bit("Air") == 0x10);
    CHECK(sim::layer_to_bit("Land") == 0x01);

    // AntiAir weapon should only target Air layer
    uint8_t aa_caps = sim::layer_to_bit("Air");
    CHECK((aa_caps & sim::layer_to_bit("Air")) != 0);
    CHECK((aa_caps & sim::layer_to_bit("Land")) == 0);

    // DirectFire should target Land and Water but not Air
    uint8_t df_caps = sim::layer_to_bit("Land") | sim::layer_to_bit("Water") | sim::layer_to_bit("Seabed");
    CHECK((df_caps & sim::layer_to_bit("Air")) == 0);
    CHECK((df_caps & sim::layer_to_bit("Land")) != 0);
}
```

- [ ] **Step 4: Build and run tests**

Run: `cmake --build build --config Debug --target osc_tests && ./build/tests/Debug/osc_tests.exe "[m158]"`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add src/sim/weapon.cpp src/lua/sim_bindings.cpp tests/test_smoke_test.cpp
git commit -m "M158a: Add weapon layer targeting filter (AntiAir, DirectFire, AntiNavy)"
```

---

### Task 7: Bombing Runs

**Files:**
- Modify: `src/sim/weapon.hpp` — add bomb_drop flag
- Modify: `src/sim/weapon.cpp` — bomb drop threshold check
- Modify: `src/sim/projectile.cpp` — ballistic gravity for bombs

- [ ] **Step 1: Add bomb drop fields to Weapon**

In `weapon.hpp`, add to Weapon struct:

```cpp
bool need_compute_bomb_drop = false; // from blueprint NeedToComputeBombDrop
f32 bomb_drop_threshold = 25.0f;     // from blueprint BombDropThreshold (default 25)
```

- [ ] **Step 2: Read bomb drop blueprint fields**

In `sim_bindings.cpp` weapon creation, read:

```cpp
// Use lua_gettable to match existing weapon blueprint read style
lua_pushstring(L, "NeedToComputeBombDrop");
lua_gettable(L, -2);
if (lua_isboolean(L, -1) && lua_toboolean(L, -1))
    weapon->need_compute_bomb_drop = true;
lua_pop(L, 1);

lua_pushstring(L, "BombDropThreshold");
lua_gettable(L, -2);
if (lua_isnumber(L, -1))
    weapon->bomb_drop_threshold = static_cast<f32>(lua_tonumber(L, -1));
lua_pop(L, 1);
```

- [ ] **Step 3: Implement bomb drop check in weapon firing**

In `weapon.cpp`, when a bomb weapon fires at a target:
- Check 2D distance to target vs bomb_drop_threshold
- Only fire when within threshold (bomber is overhead)
- Create projectile with `ballistic_accel` from blueprint (gravity, typically -4.9)
- Projectile has no tracking — purely ballistic from release altitude

```cpp
// In fire logic, if need_compute_bomb_drop:
if (need_compute_bomb_drop) {
    f32 dx = target_pos.x - owner_pos.x;
    f32 dz = target_pos.z - owner_pos.z;
    f32 horiz_dist = std::sqrt(dx * dx + dz * dz);
    if (horiz_dist > bomb_drop_threshold)
        return; // Not overhead yet — don't fire
    // Fire bomb: projectile starts at owner position, no tracking
}
```

- [ ] **Step 4: Ensure ballistic projectile applies gravity**

In `projectile.cpp`, the existing projectile update should already apply `ballistic_accel` to velocity Y. Verify the existing code in `Projectile::update()`:
- If `ballistic_accel != 0`: `velocity.y += ballistic_accel * dt; position += velocity * dt`
- If `tracking_target_id == 0`: no tracking (bomb behavior)
- Bomb projectiles should have `tracking_target_id = 0` at creation

- [ ] **Step 5: Build and verify**

Run: `cmake --build build --config Debug`
Expected: Clean build

- [ ] **Step 6: Commit**

```bash
git add src/sim/weapon.hpp src/sim/weapon.cpp src/sim/projectile.cpp src/lua/sim_bindings.cpp
git commit -m "M158b: Implement bombing runs with ballistic projectile drop"
```

---

### Task 8: Fuel System

**Files:**
- Modify: `src/sim/unit.cpp` — consume fuel per tick, auto-return on empty

- [ ] **Step 1: Add fuel consumption to Unit::update()**

In `unit.cpp`, in `Unit::update()`, after the `done_commands:` label (after command processing), add fuel consumption for airborne units:

```cpp
// Fuel consumption for air units
if (is_air_unit() && fuel_ratio_ >= 0 && fuel_use_time_ > 0) {
    fuel_ratio_ -= static_cast<f32>(dt) / fuel_use_time_;
    if (fuel_ratio_ <= 0) {
        fuel_ratio_ = 0;
        // Auto-crash: begin dying (no fuel = crash)
        if (!dying_) {
            begin_dying(3.0f); // longer death for fuel crash
        }
    }
}
```

Note: Air staging (refueling at staging platforms) and auto-return-to-base are deferred (see File Structure section). The core fuel consumption + crash-on-empty is the critical path.

Note on FuelUseTime=0: Many FA air units (scouts, etc.) have `FuelUseTime = 0` meaning infinite fuel. These will keep `fuel_ratio_ = -1` (sentinel) and the consumption check below correctly skips them.

- [ ] **Step 2: Read FuelUseTime from blueprint**

In `sim_bindings.cpp` `create_unit_core()`, verify that `fuel_use_time_` is already read from blueprint. If not, add:

```cpp
// Read Air.FuelUseTime (seconds of flight time)
if (unit->is_air_unit()) {
    // Check if already set from Physics subtable, else try Air subtable
    store->push_lua_table(*entry, L);
    lua_pushstring(L, "Physics");
    lua_rawget(L, -2);
    if (lua_istable(L, -1)) {
        lua_pushstring(L, "FuelUseTime");
        lua_rawget(L, -2);
        if (lua_isnumber(L, -1)) {
            f32 t = static_cast<f32>(lua_tonumber(L, -1));
            unit->set_fuel_use_time(t);
            if (t > 0) unit->set_fuel_ratio(1.0f); // start with full fuel
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 2);
}
```

- [ ] **Step 3: Write test**

```cpp
TEST_CASE("Air unit fuel consumption", "[m158]") {
    sim::Unit unit;
    unit.set_layer("Air");
    unit.set_fuel_use_time(10.0f); // 10 seconds
    unit.set_fuel_ratio(1.0f);     // full

    // Simulate 5 seconds of flight
    // Note: can't call unit.update() without full context, so test the math directly
    f32 ratio = 1.0f;
    for (int i = 0; i < 50; i++) {
        ratio -= 0.1f / 10.0f; // dt=0.1, fuel_use_time=10
    }
    CHECK(ratio == Catch::Approx(0.5f).margin(0.01f));
}
```

- [ ] **Step 4: Build and run tests**

Run: `cmake --build build --config Debug --target osc_tests && ./build/tests/Debug/osc_tests.exe "[m158]"`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add src/sim/unit.cpp src/lua/sim_bindings.cpp tests/test_smoke_test.cpp
git commit -m "M158c: Implement fuel consumption with crash on empty"
```

---

## Chunk 3: Air Death & Crash (M159)

### Task 9: Air Crash Physics

**Files:**
- Modify: `src/sim/unit.hpp` — add begin_air_crash() method
- Modify: `src/sim/unit.cpp` — crash physics in tick_dying(), begin_air_crash()

- [ ] **Step 1: Add begin_air_crash() to Unit**

In `unit.hpp`, add public method:

```cpp
/// Start air crash sequence (overrides normal death for air units).
void begin_air_crash(f32 crash_dmg);
```

- [ ] **Step 2: Implement begin_air_crash()**

In `unit.cpp`:

```cpp
void Unit::begin_air_crash(f32 crash_dmg) {
    if (dying_) return; // already dying
    dying_ = true;
    crashing_ = true;
    crash_damage_ = crash_dmg;
    crash_velocity_y_ = 0; // starts at zero, gravity pulls down
    // Set death_duration_ so tick_manipulators can still drive death animations
    death_duration_ = 5.0f; // generous duration for crash sequence
    death_timer_ = 5.0f;
    // Random spin for visual effect
    crash_spin_rate_ = (static_cast<f32>(entity_id() % 100) / 100.0f - 0.5f) * 4.0f; // [-2, +2] rad/s
    // Clear commands, mark do_not_target (same as begin_dying)
    clear_commands();
    set_do_not_target(true);
    // Disable economy
    economy_.consumption_mass = 0;
    economy_.consumption_energy = 0;
    economy_.consumption_active = false;
    economy_.production_mass = 0;
    economy_.production_energy = 0;
    economy_.production_active = false;
}
```

- [ ] **Step 3: Override tick_dying() for air crash physics**

In `unit.cpp`, modify `tick_dying()` to handle crash state. Current `tick_dying()` just decrements a timer. For crashing air units, apply gravity and spin instead. The Y position is tracked directly (no terrain reference needed — crash uses entity Y):

```cpp
void Unit::tick_dying(f32 dt) {
    if (crashing_) {
        // Gravity acceleration
        crash_velocity_y_ += -9.8f * dt;

        // Spin and tumble
        heading_ += crash_spin_rate_ * dt;
        bank_angle_ += crash_spin_rate_ * 0.7f * dt;
        pitch_ = std::clamp(pitch_ - 0.3f * dt, -1.0f, 0.3f);
        set_orientation(euler_to_quat(heading_, pitch_, bank_angle_));

        // Move forward (decaying) and down
        auto p = position();
        f32 fwd = current_airspeed_ * 0.5f;
        p.x += std::sin(heading_) * fwd * dt;
        p.z += std::cos(heading_) * fwd * dt;
        p.y += crash_velocity_y_ * dt;

        // Terrain impact check (Y=0 approximation; SimState will use actual terrain)
        if (p.y <= 0) {
            p.y = 0;
            set_position(p);
            crash_impacted_ = true; // signal for SimState to process splash damage
            crashing_ = false;      // stop crash physics
            return;
        }

        set_position(p);
        current_airspeed_ *= (1.0f - 0.5f * dt); // decelerate forward speed
        return;
    }

    // Normal death
    death_timer_ -= dt;
}
```

- [ ] **Step 4: Wire up air crash in entity_Destroy / begin_dying**

In `moho_bindings.cpp` where `entity_Destroy` calls `begin_dying()`, add air crash check:

```cpp
// In entity_Destroy (or wherever begin_dying is called for units):
if (unit->is_air_unit() && !unit->is_crashing()) {
    unit->begin_air_crash(unit->crash_damage());
    return 0; // Don't remove yet — crash sequence will handle it
}
```

Also modify `Unit::begin_dying()` to auto-enter crash for air units:

```cpp
void Unit::begin_dying(f32 duration) {
    if (dying_) return;
    if (is_air_unit()) {
        begin_air_crash(crash_damage_);
        return;
    }
    dying_ = true;
    death_timer_ = duration;
    death_duration_ = duration;
    clear_commands();
    set_do_not_target(true);
}
```

- [ ] **Step 5: Build and verify**

Run: `cmake --build build --config Debug`
Expected: Clean build

- [ ] **Step 6: Commit**

```bash
git add src/sim/unit.hpp src/sim/unit.cpp src/lua/moho_bindings.cpp
git commit -m "M159a: Implement air crash physics (gravity, spin, forward momentum)"
```

---

### Task 10: Crash Impact — Wreckage and Splash Damage

**Files:**
- Modify: `src/sim/sim_state.cpp` — detect crashed units, deal splash damage, spawn wreckage

- [ ] **Step 1: Add crash impact processing to SimState::tick()**

In `sim_state.cpp`, in the entity update section of `tick()`, after entities are updated, check for crashed air units that have hit the ground:

```cpp
// Process air crash impacts — check for crash_impacted_ flag set by tick_dying()
std::vector<u32> crash_impacts;
for (auto& [id, entity] : registry_.entities()) {
    if (entity->destroyed() || !entity->is_unit()) continue;
    auto* unit = static_cast<Unit*>(entity.get());
    if (unit->crash_impacted()) {
        crash_impacts.push_back(id);
    }
}

for (u32 crash_id : crash_impacts) {
    auto* ce = registry_.find(crash_id);
    if (!ce || ce->destroyed()) continue;
    auto* crash_unit = static_cast<Unit*>(ce);

    // Deal crash damage to nearby units (direct health reduction)
    f32 crash_radius = crash_unit->footprint_size_x() * 1.5f;
    if (crash_radius < 2.0f) crash_radius = 2.0f;
    f32 dmg = crash_unit->crash_damage();
    auto nearby = registry_.collect_in_radius(ce->position().x, ce->position().z, crash_radius);
    for (u32 nid : nearby) {
        if (nid == crash_id) continue;
        auto* ne = registry_.find(nid);
        if (!ne || ne->destroyed()) continue;
        f32 new_hp = ne->health() - dmg;
        ne->set_health(new_hp);
        if (new_hp <= 0 && ne->is_unit()) {
            static_cast<Unit*>(ne)->begin_dying(0.1f);
        }
    }

    // Spawn death event for visual (reuse existing DeathEvent system)
    // Use add_death_event() helper which takes individual components
    add_death_event(ce->position().x, ce->position().y, ce->position().z,
                    crash_radius, ce->army());

    // Destroy the unit
    ce->mark_destroyed();
}
```

Note: Verify `add_death_event()` signature and `health()`/`set_health()` accessors on Entity against the current codebase. The `crash_impacted_` flag is consumed once — SimState detects it and destroys the unit in the same tick.

- [ ] **Step 2: Write test**

```cpp
TEST_CASE("Air crash physics: gravity pulls unit down", "[m159]") {
    sim::Unit unit;
    unit.set_layer("Air");
    unit.set_heading(0);
    unit.set_current_airspeed(10.0f);
    unit.set_current_altitude(50.0f);
    unit.set_position({100, 50, 100});

    unit.begin_air_crash(100.0f);
    CHECK(unit.is_crashing());
    CHECK(unit.is_dying());

    // Simulate crash
    f32 prev_y = unit.position().y;
    for (int i = 0; i < 10; i++) {
        unit.tick_dying(0.1f);
        if (!unit.is_crashing()) break; // hit ground
    }

    // Should have fallen
    CHECK(unit.position().y < prev_y);
}
```

- [ ] **Step 3: Build and run tests**

Run: `cmake --build build --config Debug --target osc_tests && ./build/tests/Debug/osc_tests.exe "[m159]"`
Expected: PASS

- [ ] **Step 4: Commit**

```bash
git add src/sim/sim_state.cpp tests/test_smoke_test.cpp
git commit -m "M159b: Add crash impact damage and wreckage spawning"
```

---

### Task 11: Integration — Verify Full Air Lifecycle

**Files:**
- Test: `tests/test_smoke_test.cpp` — integration test

- [ ] **Step 1: Write integration test for full air lifecycle**

```cpp
TEST_CASE("Air unit full lifecycle: spawn, fly, die, crash", "[m159]") {
    sim::Unit unit;
    unit.set_layer("Air");
    unit.set_max_airspeed(15.0f);
    unit.set_turn_rate_rad(2.0f);
    unit.set_accel_rate(10.0f);
    unit.set_elevation_target(20.0f);
    unit.set_climb_rate(10.0f);
    unit.set_crash_damage(100.0f);
    unit.set_position({50, 20, 50});
    unit.set_current_altitude(20.0f);
    unit.set_fuel_use_time(5.0f);
    unit.set_fuel_ratio(1.0f);

    // Fly toward target
    unit.navigator().set_goal({200, 0, 200});
    for (int i = 0; i < 30; i++) {
        unit.navigator().update_air(unit, 0.1, nullptr);
    }
    CHECK(unit.position().x > 50); // moved
    CHECK(unit.current_airspeed() > 0); // has speed

    // Kill it — should crash, not vanish
    unit.begin_dying(2.0f);
    CHECK(unit.is_crashing());

    // Run crash ticks
    bool hit_ground = false;
    for (int i = 0; i < 100; i++) {
        unit.tick_dying(0.1f);
        if (!unit.is_crashing()) {
            hit_ground = true;
            break;
        }
    }
    CHECK(hit_ground);
    CHECK(unit.position().y <= 0.1f); // near ground
}
```

- [ ] **Step 2: Full build and test suite**

Run: `cmake --build build --config Debug --target osc_tests && ./build/tests/Debug/osc_tests.exe`
Expected: All tests pass

- [ ] **Step 3: Commit**

```bash
git add tests/test_smoke_test.cpp
git commit -m "M159c: Add air lifecycle integration tests"
```

---

## Exit Criteria

After all tasks complete, verify:

1. **M157**: Air units fly at blueprint elevation, bank on turns, accelerate/decelerate, heading-based steering, boids separation prevents stacking
2. **M158**: Weapons filter targets by layer (AA→air, DirectFire→ground), bombers drop ballistic bombs, fuel depletes over time and causes crash on empty
3. **M159**: Air units crash on death with gravity, spin, forward momentum; crash deals splash damage on terrain impact

Run: `cmake --build build --config Debug && ./build/tests/Debug/osc_tests.exe`
Expected: All tests pass, clean build
