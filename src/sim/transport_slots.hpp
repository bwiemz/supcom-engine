#pragma once

#include "core/types.hpp"
#include "sim/entity.hpp" // Vector3

#include <optional>
#include <vector>

namespace osc::sim {

struct BoneData;

/// A transport's blueprint fields for its slots (`Transport.*`), with Moho's
/// defaults.
struct TransportLayout {
    i32 class_generic_up_to = 0;
    i32 class2_attach_size = 2;
    i32 class3_attach_size = 6;
    i32 class4_attach_size = 1;
    i32 class_s_attach_size = 0;
};

/// A transport's attach points, and who holds them (Moho's CAiTransportImpl
/// slots, M206l). The skeleton's bones are sorted by name into lists, one per
/// cargo class. A unit's slot is a point of its class, plus, for a class with
/// an attach size, that many small "hook" bones nearest the point; it holds
/// every one of them until it is released.
class TransportSlots {
public:
    /// A unit's slot: the transport bone it hangs from, its own bone it
    /// hangs by (-1: its centre), and the bones it holds.
    struct Slot {
        u32 unit_id = 0;
        i32 bone = -1;
        i32 unit_bone = -1;
        std::vector<i32> bones;
    };

    TransportSlots(const BoneData& skeleton, const TransportLayout& layout);

    /// Whether the skeleton has any attach point. Without one, the
    /// transport's capacity is its blueprint's Class1Capacity instead.
    bool has_points() const;

    /// Whether a unit of this class could ride at all: its class has points,
    /// or hooks enough for its attach size (TransportCanCarryUnit's test).
    bool can_carry_class(i32 transport_class) const;

    /// Whether a slot for a unit of this class is free now.
    bool has_space_for(i32 transport_class) const;

    /// Reserve a free slot for the unit: the bone it hangs from, or nothing
    /// when none is free. A unit already holding one keeps it.
    std::optional<i32> assign(u32 unit_id, i32 transport_class, i32 unit_bone);

    /// The unit's slot, or null.
    const Slot* slot_of(u32 unit_id) const;

    /// Give the unit's slot up.
    void release(u32 unit_id);

    /// Every slot held, in the order they were assigned.
    const std::vector<Slot>& slots() const { return slots_; }

private:
    struct Point {
        i32 bone = -1;
        Vector3 pos{};
    };

    /// A class's points, and its hook bones and attach size
    /// (TransportFindAttachList).
    struct AttachList {
        const std::vector<Point>* points = nullptr;
        const std::vector<Point>* hooks = nullptr;
        i32 attach_size = 1;
    };
    AttachList attach_list(i32 transport_class) const;

    /// The bones a slot at `point` holds: the point itself for one bone, or
    /// the `size` hooks nearest it (GetClosestAttachPointsTo); empty when
    /// there are too few.
    std::vector<i32> slot_bones(const std::vector<Point>& hooks, const Point& point,
                                i32 size) const;

    bool reserved(const std::vector<i32>& bones) const;

    TransportLayout layout_;
    std::vector<Point> class1_, class2_, class3_, class4_, special_, generic_, launch_;
    std::vector<Slot> slots_;
};

} // namespace osc::sim
