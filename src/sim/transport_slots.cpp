#include "sim/transport_slots.hpp"

#include "sim/bone_data.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

namespace osc::sim {

TransportSlots::TransportSlots(const BoneData& skeleton, const TransportLayout& layout)
    : layout_(layout) {
    // Moho's SetUpAttachPoints: case-sensitive name tests, the first match
    // winning, so "Attachpoint_Lrg" is never class 1.
    for (i32 i = 0; i < skeleton.bone_count(); ++i) {
        const BoneInfo& bone = skeleton.bones[static_cast<size_t>(i)];
        const auto has = [&](const char* token) {
            return std::strstr(bone.name.c_str(), token) != nullptr;
        };
        const i32 generic_up_to = layout.class_generic_up_to;
        std::vector<Point>* list = nullptr;
        if (has("Launchpoint")) {
            list = &launch_;
        } else if (has("Attachpoint_Spr")) {
            list = generic_up_to < 4 ? &class4_ : &generic_;
        } else if (has("Attachpoint_Lrg")) {
            list = generic_up_to < 3 ? &class3_ : &generic_;
        } else if (has("Attachpoint_Med")) {
            list = generic_up_to < 2 ? &class2_ : &generic_;
        } else if (has("Attachpoint")) {
            list = generic_up_to < 1 ? &class1_ : &generic_;
        } else if (has("AttachSpecial")) {
            list = &special_;
        }
        if (list) list->push_back({i, bone.world_position});
    }
}

std::vector<i32> TransportSlots::launch_bones() const {
    const std::vector<Point>& points = !launch_.empty() ? launch_ : generic_;
    std::vector<i32> bones;
    bones.reserve(points.size());
    for (const Point& p : points) bones.push_back(p.bone);
    return bones;
}

bool TransportSlots::has_points() const {
    return !class1_.empty() || !class2_.empty() || !class3_.empty() || !class4_.empty() ||
           !special_.empty() || !generic_.empty();
}

bool TransportSlots::can_carry_class(i32 transport_class) const {
    if (layout_.class_generic_up_to >= transport_class && !generic_.empty()) return true;
    const auto hooks =
        static_cast<i32>(layout_.class_generic_up_to != 0 ? generic_.size() : class1_.size());
    switch (transport_class) {
    case 1: return hooks > 0;
    case 2: return layout_.class2_attach_size != 0 && hooks >= layout_.class2_attach_size;
    case 3: return layout_.class3_attach_size != 0 && hooks >= layout_.class3_attach_size;
    case 4: // strictly more, as Moho's test is
        return layout_.class4_attach_size != 0 && hooks > layout_.class4_attach_size;
    default: return false;
    }
}

TransportSlots::AttachList TransportSlots::attach_list(i32 transport_class) const {
    static const std::vector<Point> kNone;
    AttachList list{&kNone, &kNone, 1};
    if (transport_class > layout_.class_generic_up_to) {
        switch (transport_class) {
        case 1: list.points = &class1_; break;
        case 2:
            list.points = &class2_;
            list.attach_size = layout_.class2_attach_size;
            break;
        case 3:
            list.points = &class3_;
            list.attach_size = layout_.class3_attach_size;
            break;
        case 4: // Moho's switch falls through to the special class's list and
                // size; no retail unit is class 4
        case 5:
            list.points = &special_;
            list.attach_size = layout_.class_s_attach_size;
            break;
        default: // e.g. an aircraft (class 10): no points
            break;
        }
    } else {
        list.points = &generic_;
    }
    // With an attach size, the hooks are the small bones (the generic ones
    // when there are none); without one, the class's own points.
    list.hooks = list.attach_size != 0 ? (class1_.empty() ? &generic_ : &class1_) : list.points;
    return list;
}

std::vector<i32> TransportSlots::slot_bones(const std::vector<Point>& hooks, const Point& point,
                                            i32 size) const {
    if (size <= 0) return {};
    if (size == 1) return {point.bone};
    if (hooks.size() < static_cast<size_t>(size)) return {};
    // The nearest hooks. Moho's sort leaves ties in whatever order its
    // introsort makes; ordering them by bone index keeps a slot the same on
    // every platform, as a lockstep game needs.
    std::vector<std::pair<f32, i32>> by_distance;
    by_distance.reserve(hooks.size());
    for (const Point& hook : hooks) {
        const f32 dx = hook.pos.x - point.pos.x;
        const f32 dy = hook.pos.y - point.pos.y;
        const f32 dz = hook.pos.z - point.pos.z;
        by_distance.emplace_back(dx * dx + dy * dy + dz * dz, hook.bone);
    }
    std::sort(by_distance.begin(), by_distance.end());
    std::vector<i32> bones;
    bones.reserve(static_cast<size_t>(size));
    for (i32 i = 0; i < size; ++i) bones.push_back(by_distance[static_cast<size_t>(i)].second);
    return bones;
}

bool TransportSlots::reserved(const std::vector<i32>& bones) const {
    for (const Slot& slot : slots_)
        for (const i32 bone : bones)
            if (std::find(slot.bones.begin(), slot.bones.end(), bone) != slot.bones.end())
                return true;
    return false;
}

bool TransportSlots::has_space_for(i32 transport_class) const {
    const AttachList list = attach_list(transport_class);
    const i32 size = std::max(1, list.attach_size);
    for (const Point& point : *list.points) {
        const std::vector<i32> bones = slot_bones(*list.hooks, point, size);
        if (!bones.empty() && !reserved(bones)) return true;
    }
    return false;
}

std::optional<i32> TransportSlots::assign(u32 unit_id, i32 transport_class, i32 unit_bone) {
    if (const Slot* held = slot_of(unit_id)) return held->bone;
    const AttachList list = attach_list(transport_class);
    const i32 size = std::max(1, list.attach_size);
    for (const Point& point : *list.points) {
        std::vector<i32> bones = slot_bones(*list.hooks, point, size);
        if (bones.empty() || reserved(bones)) continue;
        slots_.push_back({unit_id, point.bone, unit_bone, std::move(bones)});
        return point.bone;
    }
    return std::nullopt;
}

const TransportSlots::Slot* TransportSlots::slot_of(u32 unit_id) const {
    for (const Slot& slot : slots_)
        if (slot.unit_id == unit_id) return &slot;
    return nullptr;
}

void TransportSlots::release(u32 unit_id) {
    std::erase_if(slots_, [&](const Slot& slot) { return slot.unit_id == unit_id; });
}

} // namespace osc::sim
