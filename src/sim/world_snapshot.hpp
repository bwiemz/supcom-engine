#pragma once

#include "core/types.hpp"
#include "sim/entity.hpp"

#include <array>
#include <span>
#include <vector>

namespace osc::sim {

class SimState;

using BoneMatrix = std::array<f32, 16>;

/// One entity's render pose at the end of a tick.
struct EntityPose {
    u32 id = 0;
    Vector3 position;
    Quaternion orientation;
    Vector3 beam_end;     ///< collision beams' far end (unused otherwise)
    u32 snap_serial = 0;  ///< changes when the entity teleports (see Entity::note_snap)
    u32 bone_offset = 0;  ///< into WorldSnapshot::bones
    u32 bone_count = 0;   ///< 0 = no animated pose
};

/// The world as the renderer draws it, captured once per sim tick.
struct WorldSnapshot {
    u32 tick = 0;
    std::vector<EntityPose> entities; ///< ascending id
    std::vector<BoneMatrix> bones;    ///< pooled animated poses

    const EntityPose* find(u32 id) const;
    std::span<const BoneMatrix> bones_of(const EntityPose& pose) const {
        return {bones.data() + pose.bone_offset, pose.bone_count};
    }
    void clear() {
        tick = 0;
        entities.clear();
        bones.clear();
    }
};

/// Capture `sim`'s current tick into `out`, reusing its storage. Destroyed
/// entities are left out. Ids are sequential and never reused within a
/// session, so two snapshots can be matched by id.
void capture_world(const SimState& sim, WorldSnapshot& out);

/// The last two captured ticks.
class WorldHistory {
public:
    /// The previous current becomes previous; `sim` is captured as current.
    void capture(const SimState& sim);
    /// Forget everything (a new session).
    void clear();

    const WorldSnapshot& prev() const { return snaps_[1 - cur_]; }
    const WorldSnapshot& cur() const { return snaps_[cur_]; }
    u32 ticks_captured() const { return captured_; }

private:
    std::array<WorldSnapshot, 2> snaps_;
    int cur_ = 0;
    u32 captured_ = 0;
};

/// What one frame draws: the last two ticks and how far between them the
/// frame is (0 = the previous tick, 1 = the newest). Everything the renderer
/// places at an entity reads its pose here, so a mesh and its overlays agree.
///
/// Entities the snapshots don't know (spawned since the last tick), and every
/// entity of a default-constructed view, read the live sim.
class FrameView {
public:
    FrameView() = default;
    FrameView(const WorldSnapshot* prev, const WorldSnapshot* cur, f32 alpha)
        : prev_(prev), cur_(cur), alpha_(alpha) {}

    f32 alpha() const { return alpha_; }
    const WorldSnapshot* prev() const { return prev_; }
    const WorldSnapshot* cur() const { return cur_; }

    Vector3 position(const Entity& e) const;
    Quaternion orientation(const Entity& e) const;
    Vector3 beam_end(const Entity& e) const;

    /// The entity's animated bone pose for this frame, into `out`. False
    /// when the snapshots hold no pose for it.
    bool bones(u32 id, std::vector<BoneMatrix>& out) const;

private:
    /// The two poses to blend, or nulls. `from` is null when the entity
    /// should be drawn as it is in `cur` (just spawned, or teleported).
    struct Pair {
        const EntityPose* from = nullptr;
        const EntityPose* to = nullptr;
    };
    Pair lookup(u32 id) const;

    const WorldSnapshot* prev_ = nullptr;
    const WorldSnapshot* cur_ = nullptr;
    f32 alpha_ = 1.0f;
};

} // namespace osc::sim
