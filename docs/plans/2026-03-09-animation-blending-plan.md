# M127: Animation Blending & Transitions — Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add cross-fade transitions when switching animations and formalize multi-animator bone composition with identity reset.

**Architecture:** AnimManipulator snapshots current bone matrices on PlayAnim(), then blends from snapshot→new over a configurable duration (default 0.2s). Unit resets bone matrices to identity each tick before manipulators run, ensuring clean composition when multiple animators own different bones.

**Tech Stack:** C++17, Catch2 (tests), Lua 5.0 (bindings)

---

### Task 1: Add blend state fields to AnimManipulator

**Files:**
- Modify: `src/sim/manipulator.hpp:116-128`

**Step 1: Add blend fields to AnimManipulator private section**

In `src/sim/manipulator.hpp`, add these fields after `disabled_bones_` (line 128):

```cpp
    // Cross-fade blending state
    f32 blend_time_ = 0.2f;        // default cross-fade duration (seconds)
    f32 blend_remaining_ = 0.0f;   // time left in current cross-fade (0 = no blend)
    std::vector<std::array<f32, 16>> blend_from_matrices_; // snapshot of "from" pose
```

**Step 2: Add public SetBlendTime accessor**

After line 113 (`void set_bone_enabled(...)`) add:

```cpp
    /// Set cross-fade blend duration for animation transitions.
    void set_blend_time(f32 seconds) { blend_time_ = seconds; }
    f32 blend_time() const { return blend_time_; }
```

**Step 3: Build to verify header compiles**

Run: `cmake --build build --config Debug 2>&1 | tail -5`
Expected: Build succeeds (no new code uses the fields yet)

**Step 4: Commit**

```bash
git add src/sim/manipulator.hpp
git commit -m "M127: add blend state fields to AnimManipulator"
```

---

### Task 2: Snapshot bone matrices on PlayAnim and blend during compute

**Files:**
- Modify: `src/sim/manipulator.cpp:157-184` (play_anim)
- Modify: `src/sim/manipulator.cpp:241-306` (compute_bone_matrices)

**Step 1: Add snapshot logic to play_anim()**

In `src/sim/manipulator.cpp`, modify `AnimManipulator::play_anim()`. Before the existing line `current_anim_ = anim;` (line 159), add the snapshot logic:

```cpp
void AnimManipulator::play_anim(const std::string& anim, bool loop,
                                 AnimCache* cache) {
    // Snapshot current bone matrices for cross-fade blending
    // (only if we're already playing an animation with bone data)
    if (owner_ && !current_anim_.empty() && sca_data_ && blend_time_ > 0.0f) {
        blend_from_matrices_ = owner_->animated_bone_matrices();
        blend_remaining_ = blend_time_;
    } else {
        blend_from_matrices_.clear();
        blend_remaining_ = 0.0f;
    }

    current_anim_ = anim;
    // ... rest of function unchanged ...
```

**Step 2: Add matrix lerp helper in the anonymous namespace**

After the `mat4_multiply` function (line 153), add:

```cpp
/// Component-wise linear interpolation of 4x4 matrices.
/// Sufficient for short blend durations where rotation error is imperceptible.
void mat4_lerp(f32* out, const f32* a, const f32* b, f32 t) {
    for (int i = 0; i < 16; i++) {
        out[i] = a[i] + t * (b[i] - a[i]);
    }
}
```

**Step 3: Add blend logic to compute_bone_matrices()**

At the end of `compute_bone_matrices()`, after the existing for-loop that writes `matrices[scm_idx]` (after line 305), add blend application:

```cpp
    // Apply cross-fade blending if active
    if (blend_remaining_ > 0.0f && !blend_from_matrices_.empty()) {
        f32 weight = blend_remaining_ / blend_time_; // 1.0 → 0.0 over blend duration
        for (u32 i = 0; i < num_sca_bones; i++) {
            i32 scm_idx = (i < sca_to_scm_map_.size())
                              ? sca_to_scm_map_[i] : -1;
            if (scm_idx >= 0 &&
                scm_idx < static_cast<i32>(matrices.size()) &&
                scm_idx < static_cast<i32>(blend_from_matrices_.size()) &&
                disabled_bones_.find(scm_idx) == disabled_bones_.end()) {
                f32 blended[16];
                mat4_lerp(blended,
                          matrices[scm_idx].data(),        // "to" (new anim)
                          blend_from_matrices_[scm_idx].data(), // "from" (snapshot)
                          weight);
                std::memcpy(matrices[scm_idx].data(), blended, sizeof(f32) * 16);
            }
        }
    }
```

**Step 4: Decrement blend_remaining_ in tick()**

In `AnimManipulator::tick()`, after the `compute_bone_matrices()` call (line 223), add:

```cpp
    // Advance cross-fade blend
    if (blend_remaining_ > 0.0f) {
        blend_remaining_ -= dt;
        if (blend_remaining_ < 0.0f) blend_remaining_ = 0.0f;
    }
```

**Step 5: Build to verify**

Run: `cmake --build build --config Debug 2>&1 | tail -5`
Expected: Build succeeds

**Step 6: Commit**

```bash
git add src/sim/manipulator.cpp
git commit -m "M127: implement cross-fade blending in AnimManipulator"
```

---

### Task 3: Reset bone matrices to identity before tick_manipulators

**Files:**
- Modify: `src/sim/unit.cpp:2415-2450` (tick_manipulators)

**Step 1: Add identity reset at top of tick_manipulators**

In `src/sim/unit.cpp`, at the beginning of `Unit::tick_manipulators()` (after line 2415), add:

```cpp
void Unit::tick_manipulators(f32 dt, lua_State* L) {
    // Reset bone matrices to identity before manipulators write their bones.
    // Each animator/rotator/slider writes only the bones it owns; unowned bones
    // stay at identity rather than carrying stale data from a previous tick.
    static constexpr std::array<f32, 16> IDENTITY = {
        1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    for (auto& m : animated_bone_matrices_) {
        m = IDENTITY;
    }

    for (auto& m : manipulators_) {
        // ... existing loop body unchanged ...
```

**Step 2: Build and run existing tests**

Run: `cmake --build build --config Debug && ./build/tests/Debug/osc_tests.exe 2>&1 | tail -10`
Expected: All 22 tests pass (this change doesn't affect any test-observable behavior)

**Step 3: Commit**

```bash
git add src/sim/unit.cpp
git commit -m "M127: reset bone matrices to identity before tick_manipulators"
```

---

### Task 4: Add SetBlendTime Lua binding

**Files:**
- Modify: `src/lua/moho_bindings.cpp:6910-6923`

**Step 1: Add the binding function**

Before the `animation_manipulator_methods` table (before line 6913), add:

```cpp
static int anim_SetBlendTime(lua_State* L) {
    auto* m = check_manip_base(L);
    if (m) {
        f32 seconds = static_cast<f32>(luaL_checknumber(L, 2));
        if (seconds < 0.0f) seconds = 0.0f;
        static_cast<sim::AnimManipulator*>(m)->set_blend_time(seconds);
    }
    return 0;
}
```

**Step 2: Register in method table**

Add `SetBlendTime` to the `animation_manipulator_methods` array, before the `{nullptr, nullptr}` sentinel:

```cpp
static const MethodEntry animation_manipulator_methods[] = {
    {"PlayAnim",                anim_PlayAnim},
    {"SetRate",                 anim_SetRate},
    {"SetAnimationFraction",    anim_SetAnimationFraction},
    {"GetAnimationFraction",    anim_GetAnimationFraction},
    {"GetAnimationDuration",    anim_GetAnimationDuration},
    {"GetAnimationTime",        anim_GetAnimationTime},
    {"SetAnimationTime",        anim_SetAnimationTime},
    {"SetBoneEnabled",          anim_SetBoneEnabled},
    {"SetBlendTime",            anim_SetBlendTime},
    {nullptr, nullptr},
};
```

**Step 3: Build to verify**

Run: `cmake --build build --config Debug 2>&1 | tail -5`
Expected: Build succeeds

**Step 4: Commit**

```bash
git add src/lua/moho_bindings.cpp
git commit -m "M127: add SetBlendTime Lua binding for AnimManipulator"
```

---

### Task 5: Write unit tests for blend logic

**Files:**
- Create: `tests/test_anim_blend.cpp`
- Modify: `tests/CMakeLists.txt` (add new test source)

**Step 1: Check tests/CMakeLists.txt for the source list pattern**

Read `tests/CMakeLists.txt` to find where test sources are listed, then add `test_anim_blend.cpp`.

**Step 2: Write the test file**

Create `tests/test_anim_blend.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "sim/manipulator.hpp"
#include "sim/unit.hpp"
#include "sim/bone_data.hpp"
#include "sim/sca_parser.hpp"
#include "sim/anim_cache.hpp"

using namespace osc;
using namespace osc::sim;
using Catch::Matchers::WithinAbs;

namespace {

/// Build minimal SCA data with 1 bone, 2 frames.
/// Frame 0: bone at position (0,0,0) identity rotation.
/// Frame 1: bone at position (1,0,0) identity rotation.
SCAData make_test_sca() {
    SCAData sca;
    sca.magic = {'A', 'N', 'I', 'M'};
    sca.version = 5;
    sca.num_frames = 2;
    sca.num_bones = 1;
    sca.duration = 1.0f;
    sca.bone_names = {"bone_0"};
    sca.parent_indices = {-1};

    SCAFrame f0;
    f0.time = 0.0f;
    f0.flags = 0;
    f0.bones.push_back({{0, 0, 0}, {0, 0, 0, 1}}); // pos=(0,0,0) rot=identity
    sca.frames.push_back(f0);

    SCAFrame f1;
    f1.time = 1.0f;
    f1.flags = 0;
    f1.bones.push_back({{1, 0, 0}, {0, 0, 0, 1}}); // pos=(1,0,0) rot=identity
    sca.frames.push_back(f1);

    return sca;
}

/// Build a second SCA: bone moves on Y axis instead.
/// Frame 0: position (0,0,0). Frame 1: position (0,2,0).
SCAData make_test_sca_b() {
    SCAData sca;
    sca.magic = {'A', 'N', 'I', 'M'};
    sca.version = 5;
    sca.num_frames = 2;
    sca.num_bones = 1;
    sca.duration = 1.0f;
    sca.bone_names = {"bone_0"};
    sca.parent_indices = {-1};

    SCAFrame f0;
    f0.time = 0.0f;
    f0.flags = 0;
    f0.bones.push_back({{0, 0, 0}, {0, 0, 0, 1}});
    sca.frames.push_back(f0);

    SCAFrame f1;
    f1.time = 1.0f;
    f1.flags = 0;
    f1.bones.push_back({{0, 2, 0}, {0, 0, 0, 1}});
    sca.frames.push_back(f1);

    return sca;
}

/// Minimal BoneData with one bone at origin, identity inverse_bind_pose.
BoneData make_test_bones() {
    BoneData bd;
    BoneInfo bi;
    bi.name = "bone_0";
    bi.parent_index = -1;
    bi.local_position = {0, 0, 0};
    bi.local_rotation = {0, 0, 0, 1};
    bi.world_position = {0, 0, 0};
    bi.world_rotation = {0, 0, 0, 1};
    // Identity inverse bind pose
    bi.inverse_bind_pose = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    bd.bones.push_back(bi);
    bd.name_to_index["bone_0"] = 0;
    return bd;
}

} // anonymous namespace

TEST_CASE("AnimManipulator default blend time is 0.2s", "[anim]") {
    AnimManipulator anim;
    CHECK_THAT(anim.blend_time(), WithinAbs(0.2, 0.001));
}

TEST_CASE("AnimManipulator SetBlendTime changes duration", "[anim]") {
    AnimManipulator anim;
    anim.set_blend_time(0.5f);
    CHECK_THAT(anim.blend_time(), WithinAbs(0.5, 0.001));
}

TEST_CASE("Cross-fade blends bone matrices over time", "[anim]") {
    // Setup: unit with 1 bone, 2 SCA animations
    auto bones = make_test_bones();
    auto sca_a = make_test_sca();    // moves bone on X
    auto sca_b = make_test_sca_b();  // moves bone on Y

    // Create a minimal AnimCache that serves our test SCAs
    AnimCache cache(nullptr); // no VFS needed — we inject directly
    cache.inject("anim_a", std::move(sca_a));
    cache.inject("anim_b", std::move(sca_b));

    // Create unit with bone data
    Unit unit;
    unit.set_bone_data(&bones);
    unit.init_animated_bones();
    REQUIRE(unit.animated_bone_count() == 1);

    // Create animator with 0.4s blend time
    auto manip = std::make_unique<AnimManipulator>();
    manip->set_blend_time(0.4f);
    manip->set_owner(&unit);

    // Play first animation, advance to fraction=1.0 (bone at X=1)
    manip->play_anim("anim_a", false, &cache);
    manip->set_rate(1.0f);
    manip->tick(1.0f); // advance full duration → fraction=1.0

    // Bone 0 col3 (translation) should be at (1, 0, 0)
    auto& matrices = unit.animated_bone_matrices();
    CHECK_THAT(static_cast<double>(matrices[0][12]), WithinAbs(1.0, 0.01)); // X
    CHECK_THAT(static_cast<double>(matrices[0][13]), WithinAbs(0.0, 0.01)); // Y

    // Now switch to anim_b — should trigger cross-fade
    manip->play_anim("anim_b", false, &cache);
    manip->set_rate(1.0f);

    // Tick a small amount (0.1s). Blend weight = (0.4-0.1)/0.4 = 0.75
    // "from" snapshot has bone at (1,0,0). New anim at fraction=0.1 → bone at (0.2,0,0) approx
    // But anim_b frame0=(0,0,0) frame1=(0,2,0), at fraction=0.1 → bone at (0, 0.2, 0)
    // Blended X = lerp(to_X, from_X, 0.75) = lerp(0.0, 1.0, 0.75) = 0.75
    // Blended Y = lerp(to_Y, from_Y, 0.75) = lerp(0.2, 0.0, 0.75) = 0.05
    manip->tick(0.1f);

    CHECK_THAT(static_cast<double>(matrices[0][12]), WithinAbs(0.75, 0.05)); // X blended
    CHECK_THAT(static_cast<double>(matrices[0][13]), WithinAbs(0.05, 0.05)); // Y blended

    // After blend completes (tick another 0.4s), should be pure anim_b
    manip->tick(0.4f);

    // fraction is now 0.1 + 0.4 = 0.5, so bone at (0, 1.0, 0)
    CHECK_THAT(static_cast<double>(matrices[0][12]), WithinAbs(0.0, 0.01)); // X = 0
    CHECK_THAT(static_cast<double>(matrices[0][13]), WithinAbs(1.0, 0.01)); // Y = 1
}

TEST_CASE("No blend when blend_time is 0", "[anim]") {
    auto bones = make_test_bones();
    auto sca_a = make_test_sca();
    auto sca_b = make_test_sca_b();

    AnimCache cache(nullptr);
    cache.inject("anim_a", std::move(sca_a));
    cache.inject("anim_b", std::move(sca_b));

    Unit unit;
    unit.set_bone_data(&bones);
    unit.init_animated_bones();

    auto manip = std::make_unique<AnimManipulator>();
    manip->set_blend_time(0.0f); // disable blending
    manip->set_owner(&unit);

    manip->play_anim("anim_a", false, &cache);
    manip->set_rate(1.0f);
    manip->tick(1.0f); // bone at (1,0,0)

    // Switch — no blend, immediate new anim
    manip->play_anim("anim_b", false, &cache);
    manip->set_rate(1.0f);
    manip->tick(0.1f); // fraction=0.1 → bone at (0, 0.2, 0)

    auto& matrices = unit.animated_bone_matrices();
    CHECK_THAT(static_cast<double>(matrices[0][12]), WithinAbs(0.0, 0.01)); // X = 0 (no blend)
    CHECK_THAT(static_cast<double>(matrices[0][13]), WithinAbs(0.2, 0.05)); // Y = 0.2
}

TEST_CASE("Identity reset clears stale bone data", "[anim]") {
    auto bones = make_test_bones();
    Unit unit;
    unit.set_bone_data(&bones);
    unit.init_animated_bones();

    // Manually set bone 0 to non-identity
    auto& matrices = unit.animated_bone_matrices();
    matrices[0][12] = 99.0f; // translation X

    // tick_manipulators with no manipulators should reset to identity
    unit.tick_manipulators(0.016f, nullptr);

    CHECK_THAT(static_cast<double>(matrices[0][12]), WithinAbs(0.0, 0.001));
    CHECK_THAT(static_cast<double>(matrices[0][0]),  WithinAbs(1.0, 0.001)); // identity diagonal
    CHECK_THAT(static_cast<double>(matrices[0][5]),  WithinAbs(1.0, 0.001));
    CHECK_THAT(static_cast<double>(matrices[0][10]), WithinAbs(1.0, 0.001));
    CHECK_THAT(static_cast<double>(matrices[0][15]), WithinAbs(1.0, 0.001));
}
```

**Step 3: Add AnimCache::inject() method**

The tests need an `inject()` method on AnimCache to insert test SCA data without VFS. Check `src/sim/anim_cache.hpp` — if no `inject()` method exists, add one:

```cpp
/// Inject pre-built SCA data for testing (no VFS read).
void inject(const std::string& path, SCAData data) {
    cache_[path] = std::move(data);
}
```

**Step 4: Add Unit::set_bone_data() if needed**

Check `src/sim/unit.hpp` for a setter. The tests need to set bone_data on a unit without going through the full entity creation pipeline. If `set_bone_data()` doesn't exist, add it:

```cpp
void set_bone_data(const BoneData* bd) { bone_data_ptr_ = bd; }
```

(Adjust field name to match whatever the unit uses internally to store its bone data pointer.)

**Step 5: Add test source to CMakeLists.txt**

In `tests/CMakeLists.txt`, add `test_anim_blend.cpp` to the test sources list and ensure `osc_sim` is in the link dependencies.

**Step 6: Build and run tests**

Run: `cmake --build build --config Debug && ./build/tests/Debug/osc_tests.exe "[anim]" -v 2>&1`
Expected: All 5 new tests pass

**Step 7: Run full test suite**

Run: `./build/tests/Debug/osc_tests.exe 2>&1 | tail -5`
Expected: All tests pass (22 old + 5 new = 27)

**Step 8: Commit**

```bash
git add tests/test_anim_blend.cpp tests/CMakeLists.txt src/sim/anim_cache.hpp src/sim/unit.hpp
git commit -m "M127: add unit tests for animation cross-fade blending"
```

---

### Task 6: Build and visual verification

**Step 1: Full build**

Run: `cmake --build build --config Debug 2>&1 | tail -5`
Expected: Clean build, no warnings

**Step 2: Run all tests**

Run: `./build/tests/Debug/osc_tests.exe 2>&1 | tail -5`
Expected: All tests pass

**Step 3: Visual check (manual)**

Run the engine with a map that has animated units:
```bash
MSYS_NO_PATHCONV=1 ./build/Debug/opensupcom.exe --map "/maps/SCMP_009/SCMP_009_scenario.lua"
```

Observe: units transitioning between idle/walk/attack should show smooth blending instead of instant snapping. The 0.2s default should be subtle but noticeable.

**Step 4: Final commit with milestone tag**

```bash
git add -A
git commit -m "M127: Animation blending & transitions

Cross-fade blending when switching animations (default 0.2s),
identity reset for clean multi-animator composition,
SetBlendTime Lua binding for script control."
```

---

## Summary of Changes

| File | Lines Changed | Description |
|------|:---:|---|
| `src/sim/manipulator.hpp` | ~8 | Blend fields + set_blend_time accessor |
| `src/sim/manipulator.cpp` | ~35 | Snapshot on PlayAnim, mat4_lerp, blend in compute, decrement in tick |
| `src/sim/unit.cpp` | ~6 | Identity reset at top of tick_manipulators |
| `src/lua/moho_bindings.cpp` | ~12 | SetBlendTime binding function + method table entry |
| `src/sim/anim_cache.hpp` | ~4 | inject() test helper |
| `src/sim/unit.hpp` | ~1 | set_bone_data() test helper (if needed) |
| `tests/test_anim_blend.cpp` | ~170 | 5 Catch2 tests |
| `tests/CMakeLists.txt` | ~1 | Add test source |

**Total: ~237 lines across 8 files. No renderer or shader changes.**
