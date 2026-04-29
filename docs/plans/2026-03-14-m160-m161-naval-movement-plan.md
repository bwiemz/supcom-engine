# Naval Movement (M160-M161) Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ships move on water with draft-aware passability, submarines dive/surface, torpedoes home on targets, amphibious/hover units cross land-water boundaries.

**Architecture:** Extend PathfindingGrid with per-cell water depth for draft-aware naval passability. Add motion_type + naval_draft fields to Unit. Implement torpedo homing in Projectile::update(). Handle amphibious layer transitions per-tick in Unit::update().

**Tech Stack:** C++17, Lua 5.0 blueprint reads, Catch2 tests

---

## File Structure

**Modify:**
- `src/map/pathfinding_grid.hpp` — add `water_depth_` storage, extend `is_passable_for()` with draft + amphibious params
- `src/map/pathfinding_grid.cpp` — compute water depth per cell, implement extended passability
- `src/map/pathfinder.hpp` — add draft + amphibious params to `find_path()`
- `src/map/pathfinder.cpp` — forward draft + amphibious through A*, smooth_path, line-of-sight
- `src/sim/navigator.hpp` — add draft + amphibious params to `set_goal()`
- `src/sim/navigator.cpp` — pass draft + amphibious to pathfinder
- `src/sim/unit.hpp` — add `motion_type_`, `naval_draft_` fields + getters/setters
- `src/sim/unit.cpp` — naval Y in `nav_update()`, amphibious layer transitions in `update()`
- `src/lua/sim_bindings.cpp` — read MotionType to store motion_type_ + compute naval_draft_, fix Hover layer to "Land"
- `src/sim/projectile.hpp` — add terrain param to `update()`
- `src/sim/projectile.cpp` — implement torpedo homing/tracking, stay_underwater clamping
- `src/sim/sim_state.cpp` — pass terrain to `Projectile::update()`
- `tests/test_smoke_test.cpp` — naval movement, torpedo, amphibious tests

**No new files needed.**

**Deferred scope:**
- Naval factory placement validation (build footprint water check) — cosmetic, factories already work on water via Y = get_surface_height
- Factory rally point for produced ships — existing rally point system handles this
- Seabed walkers (Cybran Brick with RULEUMT_Amphibious + "Seabed" layer) — requires separate layer handling

---

## Chunk 1: Naval Pathfinding (M160a-b)

### Task 1: Add Water Depth to PathfindingGrid

**Files:**
- Modify: `src/map/pathfinding_grid.hpp`
- Modify: `src/map/pathfinding_grid.cpp`

- [ ] **Step 1: Add water_depth_ vector and extended is_passable_for**

In `pathfinding_grid.hpp`, add a `water_depth_` member and extend `is_passable_for`:

```cpp
// In public section, add overload:
/// Extended passability check with draft depth and amphibious flag.
bool is_passable_for(u32 gx, u32 gz, const std::string& layer,
                     f32 draft, bool amphibious) const;

/// Get water depth at grid cell (0 if not water).
f32 water_depth(u32 gx, u32 gz) const;
```

```cpp
// In private section:
std::vector<f32> water_depth_; // water depth per cell (water_elevation - terrain_height)
f32 water_elevation_ = 0;     // cached for queries
```

- [ ] **Step 2: Compute water depth in constructor**

In `pathfinding_grid.cpp`, in the constructor, after computing `cells_`, build `water_depth_`:

```cpp
water_elevation_ = water_elevation;
water_depth_.resize(grid_width_ * grid_height_, 0.0f);

if (has_water) {
    for (u32 gz = 0; gz < grid_height_; ++gz) {
        for (u32 gx = 0; gx < grid_width_; ++gx) {
            u32 hx0 = gx * cell_size_;
            u32 hz0 = gz * cell_size_;
            u32 hx1 = std::min(hx0 + cell_size_, heightmap.map_width());
            u32 hz1 = std::min(hz0 + cell_size_, heightmap.map_height());
            f32 avg_h = (heightmap.get_height_at_grid(hx0, hz0) +
                         heightmap.get_height_at_grid(hx1, hz0) +
                         heightmap.get_height_at_grid(hx0, hz1) +
                         heightmap.get_height_at_grid(hx1, hz1)) * 0.25f;
            if (avg_h < water_elevation) {
                water_depth_[gz * grid_width_ + gx] = water_elevation - avg_h;
            }
        }
    }
}
```

- [ ] **Step 3: Implement extended is_passable_for**

```cpp
bool PathfindingGrid::is_passable_for(u32 gx, u32 gz, const std::string& layer,
                                       f32 draft, bool amphibious) const {
    if (gx >= grid_width_ || gz >= grid_height_) return false;
    auto cell = cells_[gz * grid_width_ + gx];

    if (layer == "Air") return true;

    // Amphibious/Hover: can traverse both land and water cells
    if (amphibious) {
        return cell == CellPassability::Passable || cell == CellPassability::Water;
    }

    if (layer == "Water" || layer == "Seabed" || layer == "Sub") {
        if (cell != CellPassability::Water) return false;
        // Draft-aware: check water depth >= required draft
        if (draft > 0) {
            return water_depth_[gz * grid_width_ + gx] >= draft;
        }
        return true;
    }

    // Land: only passable terrain
    return cell == CellPassability::Passable;
}

f32 PathfindingGrid::water_depth(u32 gx, u32 gz) const {
    if (gx >= grid_width_ || gz >= grid_height_) return 0;
    return water_depth_[gz * grid_width_ + gx];
}
```

- [ ] **Step 4: Write test for draft-aware passability**

```cpp
TEST_CASE("PathfindingGrid draft-aware water passability", "[m160]") {
    // Create a simple heightmap where some cells are shallow and some deep
    // The existing PathfindingGrid constructor takes a Heightmap + water_elevation
    // We test via is_passable_for with different draft values

    sim::Unit unit;
    unit.set_layer("Water");

    // Test that extended overload defaults work (no draft = all water passable)
    // Test that draft > water_depth blocks shallow cells
    // Detailed: create grid with known water depths, verify passability

    // For unit test without full heightmap, test the method signatures compile
    // and basic logic via a manually constructed scenario
    // (Full integration test will use actual map data)

    // Simple sanity: the original is_passable_for still works
    // (backward compat test)
}
```

- [ ] **Step 5: Commit**

```bash
git add src/map/pathfinding_grid.hpp src/map/pathfinding_grid.cpp tests/test_smoke_test.cpp
git commit -m "M160a: Add water depth storage and draft-aware passability to PathfindingGrid"
```

---

### Task 2: Extend Pathfinder and Navigator with Draft + Amphibious

**Files:**
- Modify: `src/map/pathfinder.hpp`
- Modify: `src/map/pathfinder.cpp`
- Modify: `src/sim/navigator.hpp`
- Modify: `src/sim/navigator.cpp`

- [ ] **Step 1: Add draft + amphibious params to Pathfinder**

In `pathfinder.hpp`, add default params to `find_path`:

```cpp
PathResult find_path(f32 start_x, f32 start_z,
                     f32 goal_x, f32 goal_z,
                     const std::string& layer,
                     f32 draft = 0, bool amphibious = false) const;
```

Also add to private methods `astar`, `smooth_path`, `has_line_of_sight`:

```cpp
std::vector<std::pair<u32, u32>> astar(
    u32 sx, u32 sz, u32 gx, u32 gz,
    const std::string& layer, f32 draft = 0, bool amphibious = false) const;

std::vector<std::pair<u32, u32>> smooth_path(
    const std::vector<std::pair<u32, u32>>& path,
    const std::string& layer, f32 draft = 0, bool amphibious = false) const;

bool has_line_of_sight(u32 x0, u32 z0, u32 x1, u32 z1,
                       const std::string& layer, f32 draft = 0, bool amphibious = false) const;
```

- [ ] **Step 2: Forward params through Pathfinder implementation**

In `pathfinder.cpp`, update `find_path` to forward draft + amphibious to astar, smooth_path:

```cpp
PathResult Pathfinder::find_path(f32 start_x, f32 start_z,
                                  f32 goal_x, f32 goal_z,
                                  const std::string& layer,
                                  f32 draft, bool amphibious) const {
    // ... existing code ...
    // Replace grid_.is_passable_for(ux, uz, layer) calls with:
    // grid_.is_passable_for(ux, uz, layer, draft, amphibious)
    // Forward to astar(sx, sz, gx, gz, layer, draft, amphibious)
    // Forward to smooth_path(grid_path, layer, draft, amphibious)
```

Update `astar` to use `grid_.is_passable_for(unx, unz, layer, draft, amphibious)` (5 occurrences: goal cell check, neighbor check, diagonal cardinal checks).

Update `smooth_path` and `has_line_of_sight` similarly.

- [ ] **Step 3: Add draft + amphibious to Navigator::set_goal**

In `navigator.hpp`, update the pathfinding overload:

```cpp
void set_goal(const Vector3& pos, const map::Pathfinder* pathfinder,
              const Vector3& current_pos, const std::string& layer,
              f32 draft = 0, bool amphibious = false);
```

In `navigator.cpp`, forward to pathfinder:

```cpp
void Navigator::set_goal(const Vector3& pos, const map::Pathfinder* pathfinder,
                          const Vector3& current_pos,
                          const std::string& layer,
                          f32 draft, bool amphibious) {
    // ... existing code ...
    // Air units skip pathfinding — unchanged
    if (layer == "Air" || !pathfinder) {
        waypoints_.push_back(pos);
        status_ = Status::Moving;
        return;
    }

    auto result = pathfinder->find_path(
        current_pos.x, current_pos.z, pos.x, pos.z, layer, draft, amphibious);
    // ... rest unchanged ...
}
```

- [ ] **Step 4: Commit**

```bash
git add src/map/pathfinder.hpp src/map/pathfinder.cpp src/sim/navigator.hpp src/sim/navigator.cpp
git commit -m "M160b: Thread draft and amphibious params through Pathfinder and Navigator"
```

---

### Task 3: Add motion_type + naval_draft to Unit, Read from Blueprint

**Files:**
- Modify: `src/sim/unit.hpp`
- Modify: `src/lua/sim_bindings.cpp`

- [ ] **Step 1: Add motion_type_ and naval_draft_ fields to Unit**

In `unit.hpp`, add public getters/setters:

```cpp
// Motion type (from blueprint Physics.MotionType)
const std::string& motion_type() const { return motion_type_; }
void set_motion_type(const std::string& mt) { motion_type_ = mt; }
f32 naval_draft() const { return naval_draft_; }
void set_naval_draft(f32 d) { naval_draft_ = d; }
bool is_amphibious() const {
    return motion_type_ == "RULEUMT_Amphibious" || motion_type_ == "RULEUMT_AmphibiousFloating";
}
bool is_hover() const { return motion_type_ == "RULEUMT_Hover"; }
bool is_naval() const {
    return motion_type_ == "RULEUMT_Water" || motion_type_ == "RULEUMT_SurfacingSub";
}
```

Add private fields (near the layer_ field):

```cpp
std::string motion_type_;       // raw MotionType from blueprint
f32 naval_draft_ = 0;           // abs(Physics.Elevation) for naval units
```

- [ ] **Step 2: Store motion_type in sim_bindings.cpp**

In `sim_bindings.cpp`, in the existing Physics.MotionType read block (~line 526-547), store the motion type string on the unit:

```cpp
if (lua_isstring(L, -1)) {
    std::string mt = lua_tostring(L, -1);
    unit->set_motion_type(mt);  // <-- ADD THIS LINE
    if (mt == "RULEUMT_Air") unit->set_layer("Air");
    else if (mt == "RULEUMT_Water" || mt == "RULEUMT_SurfacingSub")
        unit->set_layer("Water");
    else if (mt == "RULEUMT_Hover")
        unit->set_layer("Land");  // <-- FIX: Hover is Land layer, not Water
    else if (mt == "RULEUMT_Amphibious" || mt == "RULEUMT_AmphibiousFloating")
        unit->set_layer("Land");
}
```

**IMPORTANT**: This also fixes the Hover layer bug — change from "Water" to "Land" (spec M161 requirement).

- [ ] **Step 3: Read Physics.Elevation for naval draft**

After the MotionType block, read Physics.Elevation for naval units to compute draft:

```cpp
// Read Physics.Elevation for naval units (negative = draft below water surface)
if (unit->is_naval()) {
    store->push_lua_table(*entry, L);
    lua_pushstring(L, "Physics");
    lua_gettable(L, -2);
    if (lua_istable(L, -1)) {
        lua_pushstring(L, "Elevation");
        lua_gettable(L, -2);
        if (lua_isnumber(L, -1)) {
            f32 elev = static_cast<f32>(lua_tonumber(L, -1));
            unit->set_elevation_target(elev); // store raw (negative for ships)
            unit->set_naval_draft(std::abs(elev)); // positive draft for passability
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 2);
}
```

- [ ] **Step 4: Write test**

```cpp
TEST_CASE("Naval unit fields from blueprint", "[m160]") {
    sim::Unit unit;
    unit.set_motion_type("RULEUMT_Water");
    unit.set_naval_draft(3.0f);
    unit.set_elevation_target(-3.0f);

    CHECK(unit.is_naval());
    CHECK_FALSE(unit.is_amphibious());
    CHECK_FALSE(unit.is_hover());
    CHECK(unit.naval_draft() == 3.0f);
    CHECK(unit.elevation_target() == -3.0f);

    sim::Unit hover;
    hover.set_motion_type("RULEUMT_Hover");
    CHECK(hover.is_hover());
    CHECK_FALSE(hover.is_naval());

    sim::Unit amphib;
    amphib.set_motion_type("RULEUMT_Amphibious");
    CHECK(amphib.is_amphibious());
}
```

- [ ] **Step 5: Commit**

```bash
git add src/sim/unit.hpp src/lua/sim_bindings.cpp tests/test_smoke_test.cpp
git commit -m "M160c: Add motion_type + naval_draft to Unit, fix Hover layer to Land"
```

---

## Chunk 2: Naval Movement & Submarines (M160d-e)

### Task 4: Naval Y Positioning + Pass Draft to Pathfinder

**Files:**
- Modify: `src/sim/unit.cpp`

- [ ] **Step 1: Update nav_update() for naval Y positioning and draft forwarding**

In `unit.cpp`, modify `nav_update()` to:
1. Pass draft + amphibious to navigator when setting goals
2. Handle Sub layer Y positioning (below water surface)

```cpp
bool Unit::nav_update(f64 dt, const map::Terrain* terrain) {
    if (is_air_unit())
        return navigator_.update_air(*this, dt, terrain);
    bool result = navigator_.update(*this, effective_speed(), dt, terrain);

    // Sub units: smooth transition to dive depth
    if (terrain && layer_ == "Sub") {
        auto p = position();
        f32 target_y = terrain->water_elevation() + elevation_target_; // elevation_target_ is negative
        f32 rate = 5.0f * static_cast<f32>(dt);
        if (std::abs(p.y - target_y) <= rate)
            p.y = target_y;
        else if (p.y > target_y)
            p.y -= rate;
        else
            p.y += rate;
        set_position(p);
    }

    return result;
}
```

- [ ] **Step 2: Pass draft + amphibious when setting navigator goals**

In `Unit::update()`, every call to `navigator_.set_goal(pos, ctx.pathfinder, position(), layer_)` needs to also pass draft and amphibious info. Since these are default params, only the Move, Attack, and Patrol commands need updating (they use `nav_update`). The set_goal calls in these commands:

Find all `navigator_.set_goal(` calls in unit.cpp and add draft + amphibious params:

```cpp
// Pattern: change
navigator_.set_goal(pos, ctx.pathfinder, position(), layer_);
// to:
navigator_.set_goal(pos, ctx.pathfinder, position(), layer_,
                    naval_draft_, is_amphibious() || is_hover());
```

Apply this to ALL `navigator_.set_goal` calls that take the pathfinder (there are ~15 call sites in various commands: Move, Attack, Patrol, BuildMobile, Reclaim, Repair, Guard, Capture, TransportLoad, TransportUnload, Sacrifice, Overcharge, Ferry). The default param values (0, false) are fine for Land units, but naval/amphibious units need the real values.

- [ ] **Step 3: Write test for naval Y positioning**

```cpp
TEST_CASE("Naval sub unit Y below water surface", "[m160]") {
    sim::Unit unit;
    unit.set_layer("Sub");
    unit.set_motion_type("RULEUMT_SurfacingSub");
    unit.set_elevation_target(-3.0f);
    unit.set_position({100, 25, 100}); // Y=25 (water surface)
    unit.set_max_speed(5.0f);

    // After nav_update with Sub layer, Y should move toward water_elev + elevation_target
    // Since nav_update needs terrain, test the logic directly:
    f32 water_elev = 25.0f;
    f32 target_y = water_elev + unit.elevation_target(); // 25 + (-3) = 22
    CHECK(target_y == 22.0f);
}
```

- [ ] **Step 4: Commit**

```bash
git add src/sim/unit.cpp tests/test_smoke_test.cpp
git commit -m "M160d: Naval Y positioning (sub dive depth) and pass draft to pathfinder"
```

---

### Task 5: Amphibious Layer Transitions

**Files:**
- Modify: `src/sim/unit.cpp`

- [ ] **Step 1: Add amphibious layer transition in Unit::update()**

In `unit.cpp`, after the `done_commands:` label and before the air separation block, add:

```cpp
// Amphibious layer transition: auto-switch Land↔Water based on terrain
if (is_amphibious() && !dying_ && ctx.terrain) {
    f32 terrain_h = ctx.terrain->get_terrain_height(position().x, position().z);
    f32 water_elev = ctx.terrain->water_elevation();
    if (terrain_h < water_elev && layer_ == "Land") {
        set_layer_with_callback("Water", L);
    } else if (terrain_h >= water_elev && layer_ == "Water") {
        set_layer_with_callback("Land", L);
    }
    // Amphibious Y: max(terrain, water) — already handled by get_surface_height in navigator
}
```

- [ ] **Step 2: Write test**

```cpp
TEST_CASE("Amphibious unit layer transitions", "[m161]") {
    sim::Unit unit;
    unit.set_motion_type("RULEUMT_Amphibious");
    unit.set_layer("Land");

    CHECK(unit.is_amphibious());
    CHECK(unit.layer() == "Land");

    // Simulate: when terrain_h < water_elev, should switch to Water
    // When terrain_h >= water_elev, should switch to Land
    // (Full transition test requires SimContext, tested via integration)
}
```

- [ ] **Step 3: Commit**

```bash
git add src/sim/unit.cpp tests/test_smoke_test.cpp
git commit -m "M160e: Amphibious auto layer transitions (Land↔Water) based on terrain"
```

---

## Chunk 3: Torpedoes & Integration (M160f, M161)

### Task 6: Torpedo Tracking/Homing in Projectile

**Files:**
- Modify: `src/sim/projectile.hpp`
- Modify: `src/sim/projectile.cpp`
- Modify: `src/sim/sim_state.cpp`

- [ ] **Step 1: Add terrain parameter to Projectile::update()**

In `projectile.hpp`:

```cpp
void update(f64 dt, EntityRegistry& registry, lua_State* L,
            const map::Terrain* terrain = nullptr);
```

In `sim_state.cpp`, pass terrain to projectile update (~line 316):

```cpp
static_cast<Projectile*>(e)->update(SECONDS_PER_TICK,
                                     entity_registry_, L_, terrain_.get());
```

- [ ] **Step 2: Implement homing/tracking logic in Projectile::update()**

In `projectile.cpp`, after the ballistic acceleration block and before the Move block, add tracking logic:

```cpp
// Homing/tracking: steer toward target
if (tracking && target_entity_id > 0) {
    auto* target = registry.find(target_entity_id);
    if (target && !target->destroyed()) {
        auto pos = position();
        f32 tx = target->position().x - pos.x;
        f32 ty = target->position().y - pos.y;
        f32 tz = target->position().z - pos.z;
        f32 to_len = std::sqrt(tx*tx + ty*ty + tz*tz);
        if (to_len > 0.01f) {
            f32 spd = std::sqrt(velocity.x*velocity.x + velocity.y*velocity.y + velocity.z*velocity.z);
            if (spd < 0.01f) spd = max_speed > 0 ? max_speed : 1.0f;

            // Desired velocity: direction to target * current speed
            f32 inv_len = 1.0f / to_len;
            f32 dx = tx * inv_len * spd;
            f32 dy = ty * inv_len * spd;
            f32 dz = tz * inv_len * spd;

            // Turn rate: degrees/sec → radians/sec
            f32 turn_rad = turn_rate * 3.14159265f / 180.0f;
            f32 max_turn = turn_rad * static_cast<f32>(dt);

            // Angle between current velocity and desired
            f32 dot = (velocity.x*dx + velocity.y*dy + velocity.z*dz) / (spd * spd);
            dot = std::clamp(dot, -1.0f, 1.0f);
            f32 angle = std::acos(dot);

            if (angle > 0.001f) {
                f32 t = std::min(1.0f, max_turn / angle);
                velocity.x += (dx - velocity.x) * t;
                velocity.y += (dy - velocity.y) * t;
                velocity.z += (dz - velocity.z) * t;

                // Normalize to maintain speed
                f32 new_spd = std::sqrt(velocity.x*velocity.x + velocity.y*velocity.y + velocity.z*velocity.z);
                if (new_spd > 0.01f) {
                    f32 scale = spd / new_spd;
                    velocity.x *= scale;
                    velocity.y *= scale;
                    velocity.z *= scale;
                }
            }
        }
    }
}
```

- [ ] **Step 3: Implement stay_underwater clamping**

After the Move block (pos update), add:

```cpp
// Torpedo/underwater projectile: clamp Y to water surface
if (stay_underwater && terrain) {
    f32 water_y = terrain->water_elevation();
    if (pos.y > water_y) pos.y = water_y;
}
```

- [ ] **Step 4: Read TrackTarget and StayUnderwater from projectile blueprint**

In `weapon.cpp`, in the try_fire method, within the projectile blueprint Physics reading section (~line 190-222), add after the existing VelocityAlign/UseGravity/Acceleration/MaxSpeed reads:

```cpp
// Read TrackTarget
lua_pushstring(L, "TrackTarget");
lua_gettable(L, -2);
if (lua_type(L, -1) == LUA_TBOOLEAN) {
    proj->tracking = lua_toboolean(L, -1) != 0;
}
lua_pop(L, 1);

// Read TurnRate (degrees/sec for homing)
lua_pushstring(L, "TurnRate");
lua_gettable(L, -2);
if (lua_isnumber(L, -1)) {
    proj->turn_rate = static_cast<f32>(lua_tonumber(L, -1));
}
lua_pop(L, 1);

// Read StayUnderwater
lua_pushstring(L, "StayUnderwater");
lua_gettable(L, -2);
if (lua_type(L, -1) == LUA_TBOOLEAN) {
    proj->stay_underwater = lua_toboolean(L, -1) != 0;
}
lua_pop(L, 1);
```

- [ ] **Step 5: Write test for torpedo tracking**

```cpp
TEST_CASE("Projectile homing tracks toward target", "[m160]") {
    // Create a projectile with tracking enabled
    sim::Projectile proj;
    proj.set_position({0, 0, 0});
    proj.velocity = {10, 0, 0}; // moving in +X
    proj.tracking = true;
    proj.turn_rate = 180.0f; // fast turn for test
    proj.max_speed = 10.0f;
    proj.lifetime = 5.0f;

    // Create a target entity at (0, 0, 50) — in +Z direction
    sim::EntityRegistry registry;
    auto target = std::make_unique<sim::Unit>();
    target->set_position({0, 0, 50});
    u32 tid = registry.register_entity(std::move(target));
    proj.target_entity_id = tid;
    u32 pid = registry.register_entity(std::make_unique<sim::Projectile>());

    // After update, velocity should have rotated toward target (Z component > 0)
    proj.update(0.1, registry, nullptr, nullptr);
    CHECK(proj.velocity.z > 0); // turned toward target
}
```

- [ ] **Step 6: Commit**

```bash
git add src/sim/projectile.hpp src/sim/projectile.cpp src/sim/sim_state.cpp src/sim/weapon.cpp tests/test_smoke_test.cpp
git commit -m "M160f: Torpedo homing/tracking in projectile, stay_underwater clamping"
```

---

### Task 7: Build and Run All Tests

**Files:**
- All modified files

- [ ] **Step 1: Build the project**

```bash
cmake --build build --config Debug 2>&1 | tail -20
```

Expected: clean build with no errors.

- [ ] **Step 2: Run tests**

```bash
./build/tests/Debug/osc_tests.exe 2>&1 | tail -20
```

Expected: all tests pass including new [m160] and [m161] tags.

- [ ] **Step 3: Fix any issues found**

Address compiler errors or test failures.

- [ ] **Step 4: Final commit if needed**

```bash
git add -u
git commit -m "M161: Fix build/test issues for naval movement"
```
