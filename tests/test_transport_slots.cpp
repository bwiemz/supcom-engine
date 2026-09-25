#include <catch2/catch_test_macros.hpp>

#include "sim/bone_data.hpp"
#include "sim/transport_slots.hpp"

#include <string>
#include <utility>
#include <vector>

using namespace osc;
using namespace osc::sim;

namespace {

BoneData skeleton(const std::vector<std::pair<std::string, Vector3>>& bones) {
    BoneData data;
    data.bones.push_back({"root", {}, {}, -1, {}, {}});
    for (const auto& [name, pos] : bones) {
        BoneInfo bone;
        bone.name = name;
        bone.parent_index = 0;
        bone.world_position = pos;
        data.bones.push_back(bone);
    }
    return data;
}

/// A UEF T2 transport's shape: 14 small bones in two columns (x = -1 and +1,
/// z = 0..6), 6 medium bones each beside one pair of small ones, and 3 large
/// ones each amid four.
BoneData t2_skeleton() {
    std::vector<std::pair<std::string, Vector3>> bones;
    for (int z = 0; z < 7; ++z) {
        bones.push_back({"Left_Attachpoint0" + std::to_string(z + 1), {-1.0f, 0.0f, float(z)}});
        bones.push_back({"Right_Attachpoint0" + std::to_string(z + 1), {1.0f, 0.0f, float(z)}});
    }
    for (int i = 0; i < 3; ++i) {
        const float z = 0.5f + 2.0f * float(i);
        bones.push_back({"Attachpoint_Med_0" + std::to_string(i + 1), {-1.0f, 0.0f, z}});
        bones.push_back({"Attachpoint_Med_0" + std::to_string(i + 4), {1.0f, 0.0f, z}});
        bones.push_back({"Attachpoint_Lrg_0" + std::to_string(i + 1), {0.0f, 0.0f, z}});
    }
    return skeleton(bones);
}

/// Retail's T2 transports: medium units take 2 small bones, large ones 4.
TransportLayout t2_layout() {
    TransportLayout layout;
    layout.class3_attach_size = 4;
    return layout;
}

int fill(TransportSlots& slots, i32 transport_class, u32 first_id) {
    int n = 0;
    while (slots.assign(first_id + static_cast<u32>(n), transport_class, -1)) ++n;
    return n;
}

} // namespace

TEST_CASE("A transport's slots come from its attach bones, as Moho's", "[transport]") {
    const BoneData bones = t2_skeleton();

    SECTION("14 small units, and not a 15th") {
        TransportSlots slots(bones, t2_layout());
        CHECK(slots.has_points());
        CHECK(fill(slots, 1, 100) == 14);
        CHECK_FALSE(slots.has_space_for(1));
    }
    SECTION("6 medium units, each holding 2 small bones; 2 small ones still fit") {
        TransportSlots slots(bones, t2_layout());
        CHECK(fill(slots, 2, 100) == 6);
        CHECK(slots.slot_of(100)->bones.size() == 2);
        CHECK(fill(slots, 1, 200) == 2);
    }
    SECTION("3 large units, each holding 4 small bones") {
        TransportSlots slots(bones, t2_layout());
        CHECK(fill(slots, 3, 100) == 3);
        CHECK(slots.slot_of(100)->bones.size() == 4);
        CHECK(fill(slots, 2, 200) == 0); // every medium pair is taken
        CHECK(fill(slots, 1, 300) == 2);
    }
    SECTION("A large unit shuts out the medium points that share its bones") {
        TransportSlots slots(bones, t2_layout());
        REQUIRE(slots.assign(1, 3, -1));
        CHECK(fill(slots, 2, 100) == 4);
    }
    SECTION("A released slot is free again; a unit keeps the slot it holds") {
        TransportSlots slots(bones, t2_layout());
        const auto bone = slots.assign(1, 3, 7);
        REQUIRE(bone);
        CHECK(slots.assign(1, 3, 7) == bone);
        CHECK(slots.slot_of(1)->unit_bone == 7);
        CHECK(fill(slots, 3, 100) == 2);
        slots.release(1);
        CHECK(slots.slot_of(1) == nullptr);
        CHECK(slots.has_space_for(3));
    }
}

TEST_CASE("Transport slot rules at the edges", "[transport]") {
    SECTION("Pooled into generic points, each unit takes one") {
        TransportLayout layout = t2_layout();
        layout.class_generic_up_to = 2;
        TransportSlots slots(t2_skeleton(), layout);
        CHECK(fill(slots, 2, 100) == 20); // 14 small + 6 medium points
        CHECK(slots.slot_of(100)->bones.size() == 1);
        CHECK(fill(slots, 3, 200) == 0); // its hooks are the generic points, all held
        // Large points aren't pooled; their hooks are the nearest generic
        // points, two medium and two small each.
        TransportSlots fresh(t2_skeleton(), layout);
        CHECK(fill(fresh, 3, 200) == 3);
        CHECK(fresh.slot_of(200)->bones == std::vector<i32>{15, 16, 1, 2}); // nearest first
    }
    SECTION("Too few small bones for a large unit's hooks") {
        const BoneData bones = skeleton({{"Attachpoint01", {0, 0, 0}},
                                         {"Attachpoint02", {1, 0, 0}},
                                         {"Attachpoint03", {2, 0, 0}},
                                         {"Attachpoint_Lrg_01", {1, 0, 0}}});
        TransportSlots slots(bones, t2_layout());
        CHECK_FALSE(slots.can_carry_class(3));
        CHECK_FALSE(slots.has_space_for(3));
        CHECK(slots.can_carry_class(1));
        CHECK(slots.can_carry_class(2));
    }
    SECTION("Equally near hooks go by bone index") {
        TransportLayout layout;
        layout.class3_attach_size = 2;
        TransportSlots slots(t2_skeleton(), layout);
        REQUIRE(slots.assign(1, 3, -1));
        // Lrg_01 is as near Left01, Right01, Left02 and Right02: bones 1-4.
        CHECK(slots.slot_of(1)->bones == std::vector<i32>{1, 2});
    }
    SECTION("Names are tested case-sensitively, the first match winning") {
        CHECK_FALSE(TransportSlots(skeleton({{"attachpoint01", {}}}), {}).has_points());
        TransportLayout layout;
        layout.class3_attach_size = 1;
        TransportSlots slots(skeleton({{"Attachpoint_Lrg_01", {}}}), layout);
        CHECK(slots.has_space_for(3));
        CHECK_FALSE(slots.has_space_for(1));
    }
    SECTION("An aircraft's class has no points; class 4 takes the special ones") {
        TransportSlots slots(t2_skeleton(), t2_layout());
        CHECK_FALSE(slots.can_carry_class(10));
        CHECK_FALSE(slots.has_space_for(10));
        TransportSlots special(skeleton({{"AttachSpecial01", {}}}), {});
        CHECK(special.has_space_for(4));
        CHECK_FALSE(special.has_space_for(1));
    }
}
