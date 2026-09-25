#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "lua/lua_state.hpp"
#include "sim/anim_cache.hpp"
#include "sim/bone_data.hpp"
#include "sim/manipulator.hpp"
#include "sim/sca_parser.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"

#include <cmath>
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

TEST_CASE("A storage manipulator eases its bone with its army's stored resource", "[pose]") {
    // A mass storage's block: 0.3 down when the army's storage is empty, level when full
    // (CreateStorageManip(self, 'Block', 'MASS', 0, 0, -0.3, 0, 0, 0)).
    lua::LuaState lua;
    SimState sim(lua.raw(), nullptr);
    sim.add_army("ARMY_1", "ARMY_1");
    auto& economy = sim.get_army(0)->economy();
    economy.mass.max_storage = 1000;
    economy.mass.stored = 1000;
    economy.energy.max_storage = 1000;
    economy.energy.stored = 0;
    Unit unit;
    unit.set_army(0);
    StorageManipulator mass(&sim, true, {0, 0, -0.3f}, {0, 0, 0});
    mass.set_owner(&unit);
    CHECK(mass.current().z == -0.3f); // it starts empty

    // A tenth of the way toward full each tick, as Moho eases it.
    mass.tick(0.1f);
    CHECK_THAT(mass.current().z, WithinAbs(-0.27, 1e-6));
    for (int i = 0; i < 29; ++i) mass.tick(0.1f);
    CHECK_THAT(mass.current().z, WithinAbs(-0.3 * std::pow(0.9, 30), 1e-5));

    // An energy one follows energy, which is empty.
    StorageManipulator energy(&sim, false, {0, 0, -0.3f}, {0, 0, 0});
    energy.set_owner(&unit);
    energy.tick(0.1f);
    CHECK_THAT(energy.current().z, WithinAbs(-0.3, 1e-6));

    // Held while the unit is being built.
    const f32 held = mass.current().z;
    unit.set_is_being_built(true);
    mass.tick(0.1f);
    CHECK(mass.current().z == held);

    // An army with no storage at all reads as empty.
    unit.set_is_being_built(false);
    economy.mass.max_storage = 0;
    mass.tick(0.1f);
    CHECK_THAT(mass.current().z, WithinAbs(held * 0.9 - 0.3 * 0.1, 1e-6));

    // It moves its bone along the bone's own axes.
    PoseLocals pose;
    pose.local.resize(2);
    mass.set_bone_index(1);
    mass.apply_pose(pose);
    CHECK(pose.changed);
    CHECK_THAT(pose.local[1].position.z, WithinAbs(mass.current().z, 1e-6));
}
