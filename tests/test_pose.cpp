#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "sim/anim_cache.hpp"
#include "sim/bone_data.hpp"
#include "sim/manipulator.hpp"
#include "sim/sca_parser.hpp"
#include "sim/unit.hpp"

#include <memory>

using namespace osc;
using namespace osc::sim;
using Catch::Matchers::WithinAbs;

namespace {

/// root, and a child one unit ahead of it (+Z).
BoneData make_two_bones() {
    BoneData bd;
    BoneInfo root;
    root.name = "root";
    root.parent_index = -1;
    bd.bones.push_back(root);
    BoneInfo child;
    child.name = "child";
    child.parent_index = 0;
    child.local_position = {0, 0, 1};
    child.world_position = {0, 0, 1};
    // Inverse bind: translate by -1 along Z (column-major).
    child.inverse_bind_pose = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, -1, 1};
    bd.bones.push_back(child);
    bd.name_to_index["root"] = 0;
    bd.name_to_index["child"] = 1;
    return bd;
}

/// Two frames moving only the root, from the origin to (1, 0, 0).
SCAData root_slide() {
    SCAData sca;
    sca.num_frames = 2;
    sca.num_bones = 1;
    sca.duration = 1.0f;
    sca.bone_names.push_back("root");
    sca.parent_indices.push_back(-1);
    const Quaternion identity{0, 0, 0, 1};
    sca.frames.push_back({0.0f, {{{0, 0, 0}, identity}}});
    sca.frames.push_back({1.0f, {{{1, 0, 0}, identity}}});
    return sca;
}

} // namespace

TEST_CASE("A bone the animation leaves alone follows its animated parent", "[pose]") {
    BoneData bd = make_two_bones();
    Unit unit;
    unit.set_bone_data(&bd);
    unit.init_animated_bones();
    AnimCache cache(nullptr);
    cache.inject("/slide.sca", root_slide());

    auto& anim =
        static_cast<AnimManipulator&>(*unit.add_manipulator(std::make_unique<AnimManipulator>()));
    anim.play_anim("/slide.sca", false, &cache);
    anim.set_rate(1.0f);
    unit.tick_manipulators(1.0f, nullptr);

    const BonePose child = unit.bone_pose(1);
    CHECK_THAT(child.position.x, WithinAbs(1.0, 1e-5));
    CHECK_THAT(child.position.z, WithinAbs(1.0, 1e-5));
    // The renderer's matrix moves the child's vertices with the root.
    const auto& skin = unit.animated_bone_matrices()[1];
    CHECK_THAT(skin[12], WithinAbs(1.0, 1e-5));
    CHECK_THAT(skin[14], WithinAbs(0.0, 1e-5));
}

TEST_CASE("A rotator turns its bone on top of the animation", "[pose]") {
    BoneData bd = make_two_bones();
    Unit unit;
    unit.set_bone_data(&bd);
    unit.init_animated_bones();
    AnimCache cache(nullptr);
    cache.inject("/slide.sca", root_slide());

    auto& anim =
        static_cast<AnimManipulator&>(*unit.add_manipulator(std::make_unique<AnimManipulator>()));
    anim.play_anim("/slide.sca", false, &cache);
    anim.set_rate(1.0f);
    auto& rotator = static_cast<RotateManipulator&>(
        *unit.add_manipulator(std::make_unique<RotateManipulator>()));
    rotator.set_bone_index(1);
    rotator.set_axis('y');
    rotator.set_current_angle(90.0f);
    unit.tick_manipulators(1.0f, nullptr);

    // Still carried along by the root, and turned 90 deg about Y: its
    // forward (+Z) now points along +X.
    const BonePose child = unit.bone_pose(1);
    CHECK_THAT(child.position.x, WithinAbs(1.0, 1e-5));
    const Vector3 forward = quat_rotate(child.rotation, {0, 0, 1});
    CHECK_THAT(forward.x, WithinAbs(1.0, 1e-5));
    CHECK_THAT(forward.z, WithinAbs(0.0, 1e-5));
    // Its skinning matrix carries the turn: the local X column maps to -Z.
    const auto& skin = unit.animated_bone_matrices()[1];
    CHECK_THAT(skin[0], WithinAbs(0.0, 1e-5));
    CHECK_THAT(skin[2], WithinAbs(-1.0, 1e-5));
}

TEST_CASE("With no manipulator moving a bone the pose is the bind pose", "[pose]") {
    BoneData bd = make_two_bones();
    Unit unit;
    unit.set_bone_data(&bd);
    unit.init_animated_bones();
    auto& rotator = static_cast<RotateManipulator&>(
        *unit.add_manipulator(std::make_unique<RotateManipulator>()));
    rotator.set_bone_index(1);
    rotator.set_enabled(false);
    unit.tick_manipulators(0.1f, nullptr);

    CHECK_THAT(unit.bone_pose(1).position.z, WithinAbs(1.0, 1e-6));
    const auto& skin = unit.animated_bone_matrices()[1];
    CHECK_THAT(skin[0], WithinAbs(1.0, 1e-6));
    CHECK_THAT(skin[14], WithinAbs(0.0, 1e-6));
}

TEST_CASE("An animator plays at rate 1, and is done as Moho's is", "[pose]") {
    // Retail's FactoryUnit.FinishBuildThread: CreateAnimator(self):PlayAnim(anim),
    // then WaitFor it. Moho's starts at rate 1 and signals done with no
    // animation, at rate 0, or at a one-shot's end (faf-re).
    AnimCache cache(nullptr);
    cache.inject("/slide.sca", root_slide()); // one second long
    AnimManipulator anim;
    CHECK(anim.is_at_goal()); // nothing played

    anim.play_anim("/slide.sca", false, &cache);
    CHECK(anim.rate() == 1.0f); // PlayAnim alone plays it
    CHECK_FALSE(anim.is_at_goal());
    anim.tick(0.5f);
    CHECK_THAT(anim.animation_fraction(), WithinAbs(0.5, 1e-6));
    CHECK_FALSE(anim.is_at_goal());
    anim.tick(0.5f);
    CHECK(anim.is_at_goal()); // a one-shot at its end

    // A looping one is never done while it plays...
    anim.play_anim("/slide.sca", true, &cache);
    for (int i = 0; i < 5; ++i) anim.tick(0.5f);
    CHECK_FALSE(anim.is_at_goal());
    // ...but at rate 0 (a held pose) it is.
    anim.set_rate(0.0f);
    CHECK(anim.is_at_goal());

    // An animation that doesn't load is done at once.
    anim.set_rate(1.0f);
    anim.play_anim("/missing.sca", false, &cache);
    CHECK(anim.is_at_goal());
}
