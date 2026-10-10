#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/sim_bindings.hpp"
#include "sim/anim_cache.hpp"
#include "sim/bone_data.hpp"
#include "sim/manipulator.hpp"
#include "sim/sca_parser.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"

extern "C" {
#include <lua.h>
}

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

void set_lua_handle(lua_State* L, const char* name, Entity& e) {
    lua_newtable(L);
    lua_pushstring(L, "_c_object");
    lua_pushlightuserdata(L, &e);
    lua_rawset(L, -3);
    lua_setglobal(L, name);
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

TEST_CASE("A directional animation runs backward while its unit backs up", "[pose]") {
    // Moho's CAnimationManipulator negates the rate while the unit's
    // velocity is against its facing (SetDirectionalAnim; the Megalith's walk).
    AnimCache cache(nullptr);
    cache.inject("/slide.sca", root_slide()); // one second long
    Unit unit;
    AnimManipulator anim;
    anim.set_owner(&unit);
    anim.play_anim("/slide.sca", true, &cache);
    anim.tick(0.5f);
    REQUIRE_THAT(anim.animation_fraction(), WithinAbs(0.5, 1e-6));

    unit.note_drive(-1.0f, -1.0f, 2.0f, Unit::MotionTurn::Straight);
    anim.tick(0.25f); // backing up, but not directional: forward
    CHECK_THAT(anim.animation_fraction(), WithinAbs(0.75, 1e-6));

    anim.set_directional(true);
    anim.tick(0.25f); // now backward
    CHECK_THAT(anim.animation_fraction(), WithinAbs(0.5, 1e-6));

    unit.note_drive(1.0f, 1.0f, 2.0f, Unit::MotionTurn::Straight);
    anim.tick(0.25f); // moving ahead again: forward
    CHECK_THAT(anim.animation_fraction(), WithinAbs(0.75, 1e-6));
}

TEST_CASE("A motion-scaled animation plays at its unit's speed over its MaxSpeed", "[pose]") {
    AnimCache cache(nullptr);
    cache.inject("/slide.sca", root_slide());
    Unit unit;
    unit.set_max_speed(4.0f);
    AnimManipulator anim;
    anim.set_owner(&unit);
    anim.set_motion_scaled(true);
    anim.play_anim("/slide.sca", true, &cache);

    unit.note_tick_position();
    unit.set_position({0.1f, 0, 0});
    anim.tick(0.1f);
    CHECK_THAT(anim.animation_fraction(), WithinAbs(0.025, 1e-6));

    unit.note_tick_position();
    anim.tick(0.1f);
    CHECK_THAT(anim.animation_fraction(), WithinAbs(0.025, 1e-6));

    unit.note_tick_position();
    unit.set_orientation(euler_to_quat(0.1f, 0, 0));
    anim.tick(0.1f);
    CHECK_THAT(anim.animation_fraction(), WithinAbs(0.05, 1e-6));

    unit.note_tick_position();
    unit.set_position({0.5f, 0, 0});
    anim.tick(0.1f);
    CHECK_THAT(anim.animation_fraction(), WithinAbs(0.15, 1e-6));
}

TEST_CASE("A yaw-only aim controller is on target by its heading, whatever its pitch",
          "[pose][aim]") {
    // YawOnlyOnTarget (the Torrent's missile racks: pitch fixed at 55 deg):
    // Moho's CAimManipulator::CheckTracking skips the pitch lane's test. The
    // barrel still turns toward the target, within its arc.
    for (const bool yaw_only : {false, true}) {
        BoneData bd = make_two_bones();
        Unit unit;
        unit.set_bone_data(&bd);
        unit.init_animated_bones();
        auto& aim =
            static_cast<AimManipulator&>(*unit.add_manipulator(std::make_unique<AimManipulator>()));
        aim.set_yaw_bone(0);
        aim.set_pitch_bone(1);
        aim.set_firing_arc(-180.0f, 180.0f, 90.0f, 55.0f, 55.0f, 90.0f);
        aim.set_yaw_only_on_target(yaw_only);
        // Level, 100 ahead and 30 degrees to the side: the heading must turn.
        const f32 side = 100.0f * std::sin(0.5235988f);
        const f32 ahead = 100.0f * std::cos(0.5235988f);
        aim.set_target({side, 1.0f, ahead}, 2.0f * 0.0174533f);
        bool ever = false;
        bool early = false;
        for (int t = 0; t < 20; ++t) {
            unit.tick_manipulators(0.1f, nullptr);
            // 30 degrees at 90 a second takes 3-4 ticks
            if (t < 2 && aim.on_target()) early = true;
            ever = ever || aim.on_target();
        }
        CHECK(ever == yaw_only);
        CHECK_FALSE(early);
        CHECK_THAT(aim.pitch(), WithinAbs(55.0 * 0.0174533, 1e-3));
        CHECK_THAT(aim.heading(), WithinAbs(0.5235988, 2.0 * 0.0174533));
    }
}

TEST_CASE("AttachBoneToEntityBone pins the unit's bone to the entity", "[pose][lua]") {
    // sim/Unit.lua's debris: self:AttachBoneToEntityBone(partBone, boneProj, -1, false)
    lua::LuaState lua;
    SimState sim(lua.raw(), nullptr);
    lua::register_sim_bindings(lua, sim);
    lua::register_moho_bindings(lua, sim);
    BoneData bd = make_two_bones();
    bd.model_scale = 0.5f;
    auto owned = std::make_unique<Unit>();
    owned->set_bone_data(&bd);
    owned->init_animated_bones();
    owned->set_position({10, 0, 20});
    owned->set_orientation({0, 0.7071068f, 0, 0.7071068f});
    Unit& unit = *owned;
    sim.entity_registry().register_entity(std::move(owned));
    auto target_owned = std::make_unique<Entity>();
    target_owned->set_position({12, 3, 25});
    Entity& target = *target_owned;
    sim.entity_registry().register_entity(std::move(target_owned));
    lua_State* L = lua.raw();
    set_lua_handle(L, "u", unit);
    set_lua_handle(L, "proj", target);

    REQUIRE(lua.do_string("manip = moho.entity_methods.AttachBoneToEntityBone(u, 'child', proj, "
                          "-1, false)")
                .ok());
    REQUIRE(lua.do_string("assert(manip.Destroy)").ok());
    CHECK(target.parent_entity_id() == 0);

    unit.tick_manipulators(0.1f, L);
    Vector3 at = unit.bone_world_position(1);
    CHECK_THAT(at.x, WithinAbs(12.0, 1e-4));
    CHECK_THAT(at.y, WithinAbs(3.0, 1e-4));
    CHECK_THAT(at.z, WithinAbs(25.0, 1e-4));

    target.set_position({15, 8, 30});
    target.set_orientation({0.3826834f, 0, 0, 0.9238795f});
    unit.tick_manipulators(0.1f, L);
    at = unit.bone_world_position(1);
    CHECK_THAT(at.x, WithinAbs(15.0, 1e-4));
    CHECK_THAT(at.y, WithinAbs(8.0, 1e-4));
    CHECK_THAT(at.z, WithinAbs(30.0, 1e-4));
    const Quaternion turned = unit.bone_world_rotation(1);
    CHECK_THAT(turned.x, WithinAbs(0.3826834, 1e-4));
    CHECK_THAT(turned.w, WithinAbs(0.9238795, 1e-4));
    CHECK_THAT(unit.bone_world_position(0).x, WithinAbs(10.0, 1e-4));

    target.mark_destroyed();
    unit.tick_manipulators(0.1f, L);
    CHECK(unit.bone_world_position(1).y < -1000.0f);
}

TEST_CASE("CreateAnimator(unit, true) binds the animation's rate to the unit's motion",
          "[pose][lua]") {
    lua::LuaState lua;
    SimState sim(lua.raw(), nullptr);
    lua::register_sim_bindings(lua, sim);
    auto owned = std::make_unique<Unit>();
    Unit& unit = *owned;
    sim.entity_registry().register_entity(std::move(owned));
    set_lua_handle(lua.raw(), "u", unit);

    REQUIRE(
        lua.do_string("CreateAnimator(u, true) CreateAnimator(u) CreateAnimator(u, false)").ok());
    REQUIRE(unit.manipulators().size() == 3);
    CHECK(static_cast<const AnimManipulator&>(*unit.manipulators()[0]).motion_scaled());
    CHECK_FALSE(static_cast<const AnimManipulator&>(*unit.manipulators()[1]).motion_scaled());
    CHECK_FALSE(static_cast<const AnimManipulator&>(*unit.manipulators()[2]).motion_scaled());
}
