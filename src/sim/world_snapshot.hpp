#pragma once

#include "core/types.hpp"
#include "map/visibility_grid.hpp"
#include "sim/entity.hpp"
#include "sim/ieffect.hpp"
#include "sim/unit_command.hpp"

#include <array>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace osc::sim {

class SimState;

using BoneMatrix = std::array<f32, 16>;

/// What a unit's strategic icon shows, from its categories (in this order:
/// COMMAND, ENGINEER or CONSTRUCTION, STRUCTURE, AIR, NAVAL, LAND).
enum class IconClass : u8 { Land, Air, Naval, Engineer, Commander, Structure, Generic };

/// A queued order, as the renderer draws its line.
struct CommandRecord {
    CommandType type = CommandType::Stop;
    u32 target_id = 0;
    Vector3 target_pos;
};

/// An intel range a unit has on (the renderer rings it when selected).
struct IntelRecord {
    std::string type; ///< "Radar", "Sonar", "Omni", "Vision", ...
    f32 radius = 0;
};

/// One entity at the end of a tick: its pose, and everything the renderer
/// draws it with.
struct EntityRecord {
    u32 id = 0;

    // Pose (interpolated between two ticks by FrameView)
    Vector3 position;
    Quaternion orientation;
    Vector3 beam_end;     ///< collision beams' far end (unused otherwise)
    u32 snap_serial = 0;  ///< changes when the entity teleports (see Entity::note_snap)
    u32 bone_offset = 0;  ///< into WorldSnapshot::bones
    u32 bone_count = 0;   ///< 0 = no animated pose

    // Kind
    bool is_unit = false;
    bool is_prop = false;
    bool is_projectile = false;
    bool is_shield = false;
    bool is_collision_beam = false;
    bool beam_enabled = false;
    bool is_wreckage = false;

    i32 army = -1;
    std::string blueprint_id;
    std::string mesh_override;
    f32 scale_x = 1, scale_y = 1, scale_z = 1;
    f32 fraction_complete = 1;
    f32 health = 0, max_health = 0;

    // Units
    std::string unit_id;
    std::string custom_name;
    IconClass icon = IconClass::Generic;
    f32 footprint_size_x = 1;
    bool is_being_built = false;
    u32 build_target_id = 0;    ///< building when non-zero
    u32 reclaim_target_id = 0;
    u32 repair_target_id = 0;
    u32 capture_target_id = 0;
    f32 work_progress = 0;
    u8 vet_level = 0;
    u32 cargo_count = 0;
    i32 nuke_silo_ammo = 0;
    i32 tactical_silo_ammo = 0;
    i32 weapon_count = 0;
    u32 command_offset = 0, command_count = 0;   ///< into WorldSnapshot::commands
    u32 intel_offset = 0, intel_count = 0;       ///< into WorldSnapshot::intel
    u32 adjacent_offset = 0, adjacent_count = 0; ///< into WorldSnapshot::adjacent

    // Shields
    u32 shield_owner_id = 0;
    bool shield_on = false;
    f32 shield_size = 0;

    bool is_building() const { return build_target_id != 0; }
    bool is_reclaiming() const { return reclaim_target_id != 0; }
    bool is_repairing() const { return repair_target_id != 0; }
    bool is_capturing() const { return capture_target_id != 0; }
};

/// A visual effect (IEffect) at the end of a tick.
struct EffectRecord {
    u32 id = 0;
    EffectType type = EffectType::EMITTER_AT_ENTITY;
    std::string blueprint_path;
    u32 entity_id = 0;
    u32 target_entity_id = 0;
    f32 offset_x = 0, offset_y = 0, offset_z = 0;
    f32 scale = 1;
    i32 army = -1;
    f32 light_size = 0;
    f32 thickness = 0; ///< the THICKNESS param
    f32 length = 0;    ///< the LENGTH param
};

struct ResourceRecord {
    f64 stored = 0, max_storage = 0, income = 0, requested = 0;
};

/// One army at the end of a tick: its colour and economy.
struct ArmyRecord {
    bool valid = false; ///< the army has a brain
    bool has_color = false;
    u8 r = 0, g = 0, b = 0;
    ResourceRecord mass, energy;
    f64 mass_efficiency = 1, energy_efficiency = 1;
};

/// The world as the renderer draws it, captured once per sim tick.
struct WorldSnapshot {
    u32 tick = 0;
    std::vector<EntityRecord> entities; ///< ascending id
    std::vector<BoneMatrix> bones;      ///< pooled animated poses
    std::vector<CommandRecord> commands;
    std::vector<IntelRecord> intel;
    std::vector<u32> adjacent;
    std::vector<EffectRecord> effects;  ///< live effects, in creation order
    std::vector<ArmyRecord> armies;
    std::optional<map::VisibilityGrid> visibility;
    i32 player_result = 0; ///< SimState::player_result()

    const EntityRecord* find(u32 id) const;
    const ArmyRecord* army(i32 index) const {
        return index >= 0 && static_cast<size_t>(index) < armies.size()
                   ? &armies[static_cast<size_t>(index)]
                   : nullptr;
    }
    std::span<const BoneMatrix> bones_of(const EntityRecord& e) const {
        return {bones.data() + e.bone_offset, e.bone_count};
    }
    std::span<const CommandRecord> commands_of(const EntityRecord& e) const {
        return {commands.data() + e.command_offset, e.command_count};
    }
    std::span<const IntelRecord> intel_of(const EntityRecord& e) const {
        return {intel.data() + e.intel_offset, e.intel_count};
    }
    std::span<const u32> adjacent_of(const EntityRecord& e) const {
        return {adjacent.data() + e.adjacent_offset, e.adjacent_count};
    }
    void clear();
};

/// Capture `sim`'s current tick into `out`, reusing its storage. Destroyed
/// entities and effects are left out. Ids are sequential and never reused
/// within a session, so two snapshots can be matched by id.
void capture_world(const SimState& sim, WorldSnapshot& out);

/// The blueprints of everything in the world (meshes to preload).
std::vector<std::string> world_blueprints(const SimState& sim);

/// One-shot events the renderer shows: a unit's death flash, a camera
/// shake. The sim hands each tick's to the capture and forgets them.
struct DeathEventRecord {
    f32 x = 0, y = 0, z = 0;
    f32 scale = 1;
    i32 army = -1;
};
struct ShakeEventRecord {
    f32 x = 0, z = 0;
    f32 radius = 30;
    f32 max_shake = 1;
    f32 min_shake = 0;
};
/// Events of every tick captured since the renderer last took them.
struct WorldEvents {
    std::vector<DeathEventRecord> deaths;
    std::vector<ShakeEventRecord> shakes;
    void clear() {
        deaths.clear();
        shakes.clear();
    }
};

/// The last two captured ticks, and the events not yet shown.
class WorldHistory {
public:
    /// The previous current becomes previous; `sim` is captured as current.
    void capture(const SimState& sim);
    /// Forget everything (a new session).
    void clear();

    const WorldSnapshot& prev() const { return snaps_[1 - cur_]; }
    const WorldSnapshot& cur() const { return snaps_[cur_]; }
    u32 ticks_captured() const { return captured_; }
    /// The renderer takes these (and clears them) as it shows them.
    WorldEvents& events() { return events_; }

private:
    std::array<WorldSnapshot, 2> snaps_;
    int cur_ = 0;
    u32 captured_ = 0;
    WorldEvents events_;
};

/// What one frame draws: the last two ticks and how far between them the
/// frame is (0 = the previous tick, 1 = the newest). Everything the renderer
/// places at an entity reads its pose here, so a mesh and its overlays agree.
///
/// The renderer draws only what the snapshots hold. The Entity overloads
/// (for input, which works on the live sim) fall back to the live entity
/// when the snapshots don't know it, as does a default-constructed view.
class FrameView {
public:
    FrameView() = default;
    FrameView(const WorldSnapshot* prev, const WorldSnapshot* cur, f32 alpha)
        : prev_(prev), cur_(cur), alpha_(alpha) {}

    f32 alpha() const { return alpha_; }
    const WorldSnapshot* prev() const { return prev_; }
    const WorldSnapshot* cur() const { return cur_; }

    /// The newest tick's entities (empty without history).
    std::span<const EntityRecord> entities() const {
        return cur_ ? std::span<const EntityRecord>(cur_->entities) : std::span<const EntityRecord>();
    }
    const EntityRecord* find(u32 id) const { return cur_ ? cur_->find(id) : nullptr; }

    Vector3 position(const EntityRecord& e) const;
    Quaternion orientation(const EntityRecord& e) const;
    Vector3 beam_end(const EntityRecord& e) const;

    Vector3 position(const Entity& e) const;
    Quaternion orientation(const Entity& e) const;
    Vector3 beam_end(const Entity& e) const;

    /// The entity's animated bone pose for this frame, into `out`. False
    /// when the snapshots hold no pose for it.
    bool bones(u32 id, std::vector<BoneMatrix>& out) const;

private:
    /// The two records to blend. `from` is null when the entity should be
    /// drawn as it is in `cur` (just spawned, or teleported).
    struct Pair {
        const EntityRecord* from = nullptr;
        const EntityRecord* to = nullptr;
    };
    Pair lookup(u32 id) const;
    Pair pair_for(const EntityRecord& e) const;

    const WorldSnapshot* prev_ = nullptr;
    const WorldSnapshot* cur_ = nullptr;
    f32 alpha_ = 1.0f;
};

} // namespace osc::sim
