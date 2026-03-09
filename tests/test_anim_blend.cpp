#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/types.hpp"
#include "sim/manipulator.hpp"
#include "sim/unit.hpp"
#include "sim/bone_data.hpp"
#include "sim/anim_cache.hpp"
#include "sim/sca_parser.hpp"

using namespace osc;
using namespace osc::sim;
using Catch::Matchers::WithinAbs;

// ---------------------------------------------------------------------------
// Helpers: build minimal BoneData and SCA animations for a 1-bone skeleton
// ---------------------------------------------------------------------------

static BoneData make_one_bone() {
    BoneData bd;
    BoneInfo bi;
    bi.name = "root";
    bi.parent_index = -1;
    bi.local_position = {0, 0, 0};
    bi.local_rotation = {0, 0, 0, 1};
    bi.world_position = {0, 0, 0};
    bi.world_rotation = {0, 0, 0, 1};
    // Identity inverse bind pose
    bi.inverse_bind_pose = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    bd.bones.push_back(bi);
    bd.name_to_index["root"] = 0;
    return bd;
}

/// Build a 2-frame SCA that moves the root bone from `start` to `end`.
static SCAData make_linear_sca(const Vector3& start, const Vector3& end,
                                f32 duration) {
    SCAData sca;
    sca.num_frames = 2;
    sca.num_bones = 1;
    sca.duration = duration;
    sca.bone_names.push_back("root");
    sca.parent_indices.push_back(-1);

    Quaternion identity = {0, 0, 0, 1};

    SCAFrame f0;
    f0.time = 0.0f;
    f0.bones.push_back({start, identity});
    sca.frames.push_back(f0);

    SCAFrame f1;
    f1.time = duration;
    f1.bones.push_back({end, identity});
    sca.frames.push_back(f1);

    return sca;
}

// ---------------------------------------------------------------------------
// Test 1: Default blend time is 0.2s
// ---------------------------------------------------------------------------
TEST_CASE("AnimManipulator default blend time is 0.2s", "[anim]") {
    AnimManipulator anim;
    CHECK_THAT(anim.blend_time(), WithinAbs(0.2, 0.001));
}

// ---------------------------------------------------------------------------
// Test 2: SetBlendTime changes duration
// ---------------------------------------------------------------------------
TEST_CASE("AnimManipulator SetBlendTime changes duration", "[anim]") {
    AnimManipulator anim;
    anim.set_blend_time(0.5f);
    CHECK_THAT(anim.blend_time(), WithinAbs(0.5, 0.001));
}

// ---------------------------------------------------------------------------
// Test 3: Cross-fade blends bone matrices over time
// ---------------------------------------------------------------------------
TEST_CASE("Cross-fade blends bone matrices over time", "[anim]") {
    // Set up bone data and unit
    BoneData bd = make_one_bone();
    Unit unit;
    unit.set_bone_data(&bd);
    unit.init_animated_bones();
    REQUIRE(unit.animated_bone_count() == 1);

    // Prepare two SCA animations and inject into cache
    // anim_a: root moves from (0,0,0) to (1,0,0) over 1s
    // anim_b: root moves from (0,0,0) to (0,2,0) over 1s
    AnimCache cache(nullptr);
    cache.inject("/anim_a.sca", make_linear_sca({0, 0, 0}, {1, 0, 0}, 1.0f));
    cache.inject("/anim_b.sca", make_linear_sca({0, 0, 0}, {0, 2, 0}, 1.0f));

    // Create animator, attach to unit
    AnimManipulator anim;
    anim.set_owner(&unit);
    anim.set_bone_index(0);
    anim.set_blend_time(0.4f);

    // Play anim_a and advance to fraction=1.0 (bone at X=1)
    anim.play_anim("/anim_a.sca", false, &cache);
    anim.set_rate(1.0f);
    // Tick 1.0s to reach the end of anim_a
    anim.tick(1.0f);
    CHECK_THAT(anim.animation_fraction(), WithinAbs(1.0, 0.001));

    // Verify bone is at X=1 (column-major: translation at [12],[13],[14])
    {
        auto& mat = unit.animated_bone_matrices()[0];
        CHECK_THAT(static_cast<double>(mat[12]), WithinAbs(1.0, 0.01));
        CHECK_THAT(static_cast<double>(mat[13]), WithinAbs(0.0, 0.01));
    }

    // Switch to anim_b — this should snapshot the current pose and start blending
    anim.play_anim("/anim_b.sca", false, &cache);
    anim.set_rate(1.0f);

    // Tick 0.1s into the blend.
    // Inside tick: compute_bone_matrices() runs BEFORE blend_remaining_ is decremented.
    // So on this first tick, blend_remaining_=0.4, weight=0.4/0.4=1.0 (pure "from" snapshot).
    // After compute, blend_remaining_ decremented to 0.3.
    anim.tick(0.1f);

    // weight=1.0 during compute => output is entirely the snapshot: X=1, Y=0
    {
        auto& mat = unit.animated_bone_matrices()[0];
        CHECK_THAT(static_cast<double>(mat[12]), WithinAbs(1.0, 0.01));
        CHECK_THAT(static_cast<double>(mat[13]), WithinAbs(0.0, 0.01));
    }

    // Tick 0.2s more (total 0.3s into anim_b, blend_remaining_=0.3 at compute time)
    // fraction=0.3, anim_b Y=0.3*2=0.6, weight=0.3/0.4=0.75
    // Blended X = 0 + 0.75*1 = 0.75, Y = 0.6 + 0.75*(0-0.6) = 0.15
    anim.tick(0.2f);
    {
        auto& mat = unit.animated_bone_matrices()[0];
        CHECK_THAT(static_cast<double>(mat[12]), WithinAbs(0.75, 0.05));
        CHECK_THAT(static_cast<double>(mat[13]), WithinAbs(0.15, 0.05));
    }

    // Advance past blend completion: tick 0.3s more (total 0.6s, blend_remaining_
    // was 0.1 at start of this tick, so compute uses weight=0.1/0.4=0.25,
    // then blend_remaining_ goes to 0). One more tick to get pure output.
    anim.tick(0.3f);
    // Now blend_remaining_ = 0, but the compute during that tick still used
    // a small weight. Tick once more to get truly unblended output.
    anim.tick(0.1f);

    // Pure anim_b at fraction=0.7 => Y=0.7*2=1.4, X=0
    {
        auto& mat = unit.animated_bone_matrices()[0];
        CHECK_THAT(static_cast<double>(mat[12]), WithinAbs(0.0, 0.01));
        CHECK_THAT(static_cast<double>(mat[13]), WithinAbs(1.4, 0.05));
    }
}

// ---------------------------------------------------------------------------
// Test 4: No blend when blend_time is 0
// ---------------------------------------------------------------------------
TEST_CASE("No blend when blend_time is 0", "[anim]") {
    BoneData bd = make_one_bone();
    Unit unit;
    unit.set_bone_data(&bd);
    unit.init_animated_bones();

    AnimCache cache(nullptr);
    cache.inject("/anim_a.sca", make_linear_sca({0, 0, 0}, {1, 0, 0}, 1.0f));
    cache.inject("/anim_b.sca", make_linear_sca({0, 0, 0}, {0, 2, 0}, 1.0f));

    AnimManipulator anim;
    anim.set_owner(&unit);
    anim.set_bone_index(0);
    anim.set_blend_time(0.0f); // No blending

    // Play anim_a to completion
    anim.play_anim("/anim_a.sca", false, &cache);
    anim.set_rate(1.0f);
    anim.tick(1.0f);

    // Switch to anim_b with zero blend time
    anim.play_anim("/anim_b.sca", false, &cache);
    anim.set_rate(1.0f);
    anim.tick(0.1f);

    // Pure anim_b at fraction=0.1 => Y=0.2, X=0 (no blending from old pose)
    {
        auto& mat = unit.animated_bone_matrices()[0];
        CHECK_THAT(static_cast<double>(mat[12]), WithinAbs(0.0, 0.01));
        CHECK_THAT(static_cast<double>(mat[13]), WithinAbs(0.2, 0.05));
    }
}

// ---------------------------------------------------------------------------
// Test 5: Identity reset clears stale bone data
// ---------------------------------------------------------------------------
TEST_CASE("Identity reset clears stale bone data", "[anim]") {
    BoneData bd = make_one_bone();
    Unit unit;
    unit.set_bone_data(&bd);
    unit.init_animated_bones();
    REQUIRE(unit.animated_bone_count() == 1);

    // Manually set bone matrix to non-identity (put garbage translation)
    auto& mat = unit.animated_bone_matrices()[0];
    mat[12] = 99.0f;
    mat[13] = 88.0f;
    mat[14] = 77.0f;

    // tick_manipulators with no manipulators should reset to identity
    unit.tick_manipulators(0.1f, nullptr);

    // Verify identity matrix: diagonal ones, everything else zero
    auto& result = unit.animated_bone_matrices()[0];
    // Translation should be zeroed
    CHECK_THAT(static_cast<double>(result[12]), WithinAbs(0.0, 0.001));
    CHECK_THAT(static_cast<double>(result[13]), WithinAbs(0.0, 0.001));
    CHECK_THAT(static_cast<double>(result[14]), WithinAbs(0.0, 0.001));
    // Diagonal should be 1
    CHECK_THAT(static_cast<double>(result[0]),  WithinAbs(1.0, 0.001));
    CHECK_THAT(static_cast<double>(result[5]),  WithinAbs(1.0, 0.001));
    CHECK_THAT(static_cast<double>(result[10]), WithinAbs(1.0, 0.001));
    CHECK_THAT(static_cast<double>(result[15]), WithinAbs(1.0, 0.001));
}
