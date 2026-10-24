#pragma once

#include "core/types.hpp"
#include "sim/collision_cells.hpp"
#include "sim/sim_random.hpp"

#include <algorithm>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <map>
#include <unordered_map>
#include <vector>

namespace osc::sim {

class Entity;

class EntityRegistry {
    friend struct StateIO; // snapshots (state_io.hpp)
public:
    static constexpr u32 CELL_SIZE = 32;
    /// Props have their own, finer grid: a forest puts hundreds in a CELL_SIZE cell.
    static constexpr u32 PROP_CELL_SIZE = 4;

    EntityRegistry();
    ~EntityRegistry();

    /// Register an entity and assign it a unique ID. Returns the ID.
    ///
    /// Ids are Moho's (EntityDB::DoReserveId, 0x00684480), plus one so that 0 stays
    /// "none": a family and source in the high bits (a unit its army << 20; a
    /// projectile (army | 0x100) << 20; a prop 0x2FF << 20; a shield
    /// (army | 0x400) << 20; any other (army | 0x500) << 20; no army is 255), and the
    /// next serial of that pool (Moho reuses released ones; see reserve_id).
    u32 register_entity(std::unique_ptr<Entity> entity);
    /// Moho's id of the entity the engine numbers `id`.
    static u32 moho_id(u32 id) { return id - 1; }
    /// The engine's number of the entity Moho numbers `moho`.
    static u32 from_moho_id(u32 moho) { return moho + 1; }

    /// Remove an entity by ID: Moho's EntityDB::Purge. The deletion queue
    /// (destroy, purge_destroyed) comes here, so the unregister hook sees each
    /// entity exactly once, while still valid.
    void unregister_entity(u32 id);

    /// Moho's Entity::Destroy: the entity is marked (BeenDestroyed) and queued, once, and stays
    /// registered, found and in the grids until the queue is run at the end of the beat.
    void destroy(Entity& entity);
    /// The next entity of the deletion queue, first in first out; 0 when it is empty.
    u32 take_destroyed();
    /// Unregister everything the deletion queue holds, in its order (a sim runs OnDestroy first).
    void purge_destroyed();

    /// Moho's stage A list (CTaskStage): the order the entities step in, kept from walk to walk.
    std::vector<u32>& walk() { return walk_; }
    const std::vector<u32>& walk() const { return walk_; }
    void wake(Entity& entity);

    /// Called for each entity just before it is removed. SimState uses it to
    /// release what the entity owns outside the registry (its Lua table's
    /// pointer back to C++, a structure's pathfinding footprint).
    using UnregisterHook = std::function<void(Entity&)>;
    void set_unregister_hook(UnregisterHook hook) { unregister_hook_ = std::move(hook); }

    /// Look up an entity by ID. Returns nullptr if not found.
    Entity* find(u32 id) const;

    /// Whether `e` is a registered entity, asked without reading it: a Lua
    /// handle's pointer may be a platoon's, a brain's, or a freed entity's.
    bool holds(const void* e) const { return live_.count(e) != 0; }
    /// Whether `e` is an entity unregistered since the last collect_garbage
    /// (its memory, and a script's handle to it, live to the beat's end).
    bool departed(const void* e) const {
        for (const auto& dead : graveyard_)
            if (dead.get() == e) return true;
        return false;
    }

    /// Number of active entities.
    size_t count() const { return live_count_; }

    /// Free the entities unregistered since the last call. An unregistered
    /// entity leaves every lookup immediately but its memory lives until
    /// here: scripts routinely destroy a unit from inside one of its own
    /// callbacks (a structure finishing its upgrade replaces itself), and the
    /// C++ frame that ran the callback is still inside that unit's update.
    /// SimState calls this at the end of each tick, with `release` to cut
    /// each one's Lua handle first (as Moho's deletion queue, drained at the
    /// end of the beat, frees a destroyed entity). `release` may run scripts;
    /// what they unregister is freed the next time.
    void collect_garbage(const std::function<void(Entity&)>& release = {});

    /// Initialize spatial hash grid. Must be called after map dimensions are known.
    /// If not called, collect_in_radius/collect_in_rect fall back to O(N) scan.
    void init_spatial_grid(u32 map_width, u32 map_height);

    /// Called by Entity::advance_coords when its committed transform changes.
    void notify_moved(Entity& entity, bool moved);

    /// Moho's collision cells, for its rect queries (GetUnitsInRect,
    /// GetReclaimablesInRect). init_spatial_grid sizes them.
    CollisionCells& collision_cells() { return cells_; }
    /// Commit every entity's move, in id order (Moho's AdvanceCoords over its coordinate list).
    void advance_coords();

    /// Collect entity IDs within radius of a point (2D distance, ignoring Y),
    /// in ascending id order.
    std::vector<u32> collect_in_radius(f32 x, f32 z, f32 radius) const;

    /// The live units within radius of a point (2D, ignoring Y), in ascending
    /// id order: collect_in_radius's units, from a grid of units alone (a
    /// map's props aren't walked), as the units themselves. The pointers
    /// stay valid until collect_garbage() (the end of the beat), which frees
    /// what was unregistered meanwhile.
    std::vector<Entity*> units_in_radius(f32 x, f32 z, f32 radius) const;

    /// Collect entity IDs within an axis-aligned rectangle (2D, ignoring Y),
    /// in ascending id order.
    std::vector<u32> collect_in_rect(f32 x0, f32 z0, f32 x1, f32 z1) const;

    /// The live units within an axis-aligned rectangle, in ascending id
    /// order: collect_in_rect's units, from the grid of units alone (as
    /// units_in_radius, whose pointers' lifetime they share).
    std::vector<Entity*> units_in_rect(f32 x0, f32 z0, f32 x1, f32 z1) const;

    /// Every live entity within an axis-aligned rectangle (collect_in_rect's
    /// entities), each once but in no fixed order, with no list made: for a
    /// question whose answer doesn't depend on the order (a nearest one with
    /// ties broken by id). `units_only` visits the grid of units alone.
    template <typename F>
    void for_each_in_rect(f32 x0, f32 z0, f32 x1, f32 z1, bool units_only, F&& fn) const {
        if (x0 > x1) std::swap(x0, x1);
        if (z0 > z1) std::swap(z0, z1);
        const auto within = [&](const Entity& e) { return live_within(e, x0, z0, x1, z1); };
        if (!grid_initialized_) {
            const auto visit = [&](Entity& e) {
                if (within(e)) fn(e);
            };
            if (units_only) for_each_unit(visit);
            else for_each(visit);
            return;
        }
        i32 cx_min, cz_min, cx_max, cz_max;
        world_to_cell(x0, z0, cx_min, cz_min);
        world_to_cell(x1, z1, cx_max, cz_max);
        for (i32 cz = cz_min; cz <= cz_max; ++cz) {
            for (i32 cx = cx_min; cx <= cx_max; ++cx) {
                const size_t cell = cell_index(cx, cz);
                if (units_only) {
                    for (const UnitRef& unit : unit_cells_[cell])
                        if (within(*unit.entity)) fn(*unit.entity);
                } else {
                    for (const u32 id : grid_cells_[cell])
                        if (Entity* e = find(id); e && within(*e)) fn(*e);
                }
            }
        }
    }

    /// Shapes reaching this far from their entity are found by the grid;
    /// larger ones (shields, experimentals) are kept on their own list.
    static constexpr f32 COLLIDER_REACH = 8.0f;

    /// Into `out`, in ascending id order: every entity with a collision
    /// shape that could meet the segment from (x0, z0) to (x1, z1).
    void collect_colliders(f32 x0, f32 z0, f32 x1, f32 z1, std::vector<u32>& out) const;

    /// Whether `visit` says yes to a live unit with a collision shape that
    /// could reach the rectangle [x0, x1] x [z0, z1] (its position within
    /// the shape's reach of it, as collision_reach bounds the shape's box
    /// turned any way), stopping at the first yes. A unit farther away is
    /// never offered: `visit` must say no to a shape that can't reach the
    /// rectangle. The units come in no fixed order and may come twice:
    /// only for a question whose answer depends on neither (is any unit in
    /// the way?).
    template <typename F> bool any_unit_collider(f32 x0, f32 z0, f32 x1, f32 z1, F&& visit) const {
        using Visit = std::remove_reference_t<F>;
        return any_unit_collider_impl(
            x0, z0, x1, z1,
            [](const Entity& e, void* ctx) { return (*static_cast<Visit*>(ctx))(e); },
            const_cast<void*>(static_cast<const void*>(&visit)));
    }

    /// Into `out`, in ascending id order: every live prop with a collision
    /// shape that could reach the rectangle [x0, x1] x [z0, z1].
    void props_touching(f32 x0, f32 z0, f32 x1, f32 z1, std::vector<Entity*>& out) const;

    /// Called by Entity::set_collision_shape.
    void notify_collision_shape_changed(Entity& entity);

    /// Iterate all entities in id order: the same order on every platform,
    /// as lockstep needs (a hash map's order is the standard library's).
    /// Entities created during one are visited by the next;
    /// entities removed during it are skipped.
    template <typename F>
    void for_each(F&& fn) const {
        const Walking guard(walking_);
        const size_t n = order_.size();
        for (size_t i = 0; i < n; ++i)
            if (Entity* e = order_[i].entity) fn(*e);
    }

    /// Iterate the units alone, in id order, with for_each's guarantees. Most
    /// of a map's entities are props, which every per-tick unit walk (economy,
    /// intel, the AI's queries) would otherwise step over.
    template <typename F> void for_each_unit(F&& fn) const {
        const Walking guard(walking_);
        const size_t n = unit_order_.size();
        for (size_t i = 0; i < n; ++i)
            if (Entity* e = unit_order_[i].entity) fn(*e);
    }

    u32 grid_width() const { return grid_width_; }
    u32 grid_height() const { return grid_height_; }
    bool grid_initialized() const { return grid_initialized_; }

    /// Deterministic sim RNG used by sim code (e.g. weapon firing randomness).
    /// SimState points this at its own SimRandom so every client shares one
    /// seeded stream; a bare registry (unit tests) falls back to its own.
    void set_sim_random(SimRandom* r) { sim_random_ = r ? r : &default_random_; }
    SimRandom& sim_random() { return *sim_random_; }

    void set_tick_source(const u32* tick) { tick_ = tick; }
    u32 tick() const { return tick_ ? *tick_ : 0; }

private:
    /// Every live entity by its id: looked up, never walked (order_ is the order).
    std::unordered_map<u32, std::unique_ptr<Entity>> entities_;
    size_t live_count_ = 0;
    std::unordered_set<const void*> live_; ///< the registered entities, for holds()
    /// Every live entity in the order it was made (a new one appends).
    /// A removed entity leaves a null slot until compact(), so a walk in
    /// progress keeps its place.
    struct Slot {
        u32 id;
        Entity* entity;
    };
    std::vector<Slot> order_;
    std::vector<Slot> unit_order_; ///< the units of order_, for for_each_unit
    size_t removed_slots_ = 0;     ///< null slots in order_ (and so in unit_order_)
    /// Each live entity's slot in order_ and in unit_order_ (npos for a non-unit).
    std::unordered_map<u32, std::pair<size_t, size_t>> slot_of_;
    static constexpr size_t kNoSlot = static_cast<size_t>(-1);
    /// Moho's id pools, by family and source bits: each one's next serial.
    struct IdPool {
        u32 next = 0;
    };
    std::map<u32, IdPool> id_pools_;
    u32 reserve_id(const Entity& e);
    void index_slots();
    /// Walks in progress (a walk's callback may start another).
    mutable u32 walking_ = 0;
    struct Walking {
        u32& depth;
        explicit Walking(u32& d) : depth(d) { ++depth; }
        ~Walking() { --depth; }
        Walking(const Walking&) = delete;
        Walking& operator=(const Walking&) = delete;
    };
    void compact();
    std::vector<std::unique_ptr<Entity>> graveyard_; ///< see collect_garbage()
    std::deque<u32> deletion_queue_;
    std::vector<u32> walk_;
    UnregisterHook unregister_hook_;

    SimRandom default_random_;
    SimRandom* sim_random_ = &default_random_;
    const u32* tick_ = nullptr;

    /// Not destroyed, and in [x0, x1] x [z0, z1] (for_each_in_rect).
    static bool live_within(const Entity& e, f32 x0, f32 z0, f32 x1, f32 z1);

    // Spatial hash grid
    bool grid_initialized_ = false;
    u32 grid_width_ = 0;
    u32 grid_height_ = 0;
    std::vector<std::vector<u32>> grid_cells_;
    /// grid_cells_' units, with their entities.
    struct UnitRef {
        u32 id;
        Entity* entity;
    };
    std::vector<std::vector<UnitRef>> unit_cells_;
    u32 prop_grid_width_ = 0;
    u32 prop_grid_height_ = 0;
    std::vector<std::vector<UnitRef>> prop_cells_;
    /// Entities whose shape reaches beyond COLLIDER_REACH, in id order.
    std::set<u32> large_colliders_;
    /// Units and props by the cells their collision boxes cover. An entity
    /// leaves as collect_garbage frees it, not as it is unregistered: Moho's
    /// gathers find a destroyed one until the end of the beat.
    CollisionCells cells_;

    bool any_unit_collider_impl(f32 x0, f32 z0, f32 x1, f32 z1, bool (*visit)(const Entity&, void*),
                                void* ctx) const;
    void world_to_cell(f32 wx, f32 wz, i32& cx, i32& cz) const;
    size_t cell_index(i32 cx, i32 cz) const;
    void grid_insert(Entity& entity, i32 cx, i32 cz);
    void grid_remove(const Entity& entity, i32 cx, i32 cz);
    i32 prop_cell_index(f32 wx, f32 wz) const;
    void prop_cell_update(Entity& entity);
    void prop_cell_remove(Entity& entity);
};

} // namespace osc::sim
