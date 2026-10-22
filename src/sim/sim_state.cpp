#include "sim/sim_state.hpp"
#include "sim/fnv.hpp"
#include "blueprints/blueprint_store.hpp"
#include "sim/steering.hpp"
#include "sim/blueprint_categories.hpp"
#include "sim/collision_beam.hpp"
#include "sim/decal.hpp"
#include "sim/path_tables.hpp"
#include "sim/manipulator.hpp"
#include "sim/platoon.hpp"
#include "sim/formation.hpp"
#include "sim/build_info.hpp"
#include "sim/anim_cache.hpp"
#include "sim/bone_cache.hpp"
#include "audio/sound_manager.hpp"
#include "core/profiler.hpp"
#include "core/test_status.hpp"
#include "map/pathfinder.hpp"
#include "map/pathfinding_grid.hpp"
#include "map/terrain.hpp"
#include "map/intel_grid.hpp"
#include "sim/entity.hpp"
#include "sim/projectile.hpp"
#include "sim/prop.hpp"
#include "sim/prop_collision.hpp"
#include "sim/script_class.hpp"
#include "sim/shield.hpp"
#include "sim/unit.hpp"
#include "sim/world_snapshot.hpp"

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

#include <spdlog/spdlog.h>

#include <algorithm>
#include <string_view>
#include <unordered_map>
#include <bit>
#include <ostream>
#include <string>
#include <cmath>
#include <cctype>
#include <cstring>
#include <map>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace osc::sim {

u32 SimState::s_sim_generation_ = 0;

SimState::SimState(lua_State* L, blueprints::BlueprintStore* store)
    : L_(L), thread_manager_(L), blueprint_store_(store) {
    ++s_sim_generation_;
    // Sim code (e.g. weapons) draws randomness from this seeded stream via the
    // registry, so every lockstep client rolls identically.
    entity_registry_.set_sim_random(&sim_random_);
    entity_registry_.set_tick_source(&tick_count_);
    entity_registry_.set_unregister_hook(
        [this](Entity& entity) { on_entity_unregistered(entity); });
}

void SimState::occupy_footprint(Unit& unit) {
    if (!pathfinding_grid_ || !unit.has_category("STRUCTURE") ||
        unit.footprint_size_x() <= 0 || unit.footprint_size_z() <= 0) {
        return;
    }
    const Footprint fp{unit.position().x, unit.position().z,
                       unit.footprint_size_x(), unit.footprint_size_z()};
    if (!occupied_footprints_.emplace(unit.entity_id(), fp).second) return;
    pathfinding_grid_->mark_obstacle(fp.x, fp.z, fp.size_x, fp.size_z);
}

void SimState::occupy_ground(u32 id, GroundOccupant occupant) {
    release_ground(id);
    for (const OccupancyRect& r : occupant.rects) {
        occupancy_.fill(occupant.caps, r, true);
        // Moho's ExecuteOccupy dirties the clusters even for no caps.
        if (path_tables_) path_tables_->dirty(r);
    }
    ground_occupants_[id] = std::move(occupant);
}

void SimState::release_ground(u32 id) {
    const auto it = ground_occupants_.find(id);
    if (it == ground_occupants_.end()) return;
    const GroundOccupant gone = std::move(it->second);
    ground_occupants_.erase(it);
    for (const OccupancyRect& r : gone.rects) {
        occupancy_.fill(gone.caps, r, false);
        if (path_tables_) path_tables_->dirty(r);
    }
    // What still stands on any of those cells claims them again.
    const auto overlaps = [](const OccupancyRect& a, const OccupancyRect& b) {
        return a.x0 < b.x1 && b.x0 < a.x1 && a.z0 < b.z1 && b.z0 < a.z1;
    };
    for (const auto& [other_id, other] : ground_occupants_) {
        for (const OccupancyRect& r : other.rects) {
            const bool shared = std::any_of(gone.rects.begin(), gone.rects.end(),
                                            [&](const OccupancyRect& g) { return overlaps(g, r); });
            if (shared) occupancy_.fill(other.caps, r, true);
        }
    }
}

OccupancyRect SimState::move_bounds(i32 army, bool whole_map_if_allowed) {
    const OccupancyRect whole{0, 0, terrain_ ? static_cast<i32>(terrain_->map_width()) : 0,
                              terrain_ ? static_cast<i32>(terrain_->map_height()) : 0};
    if (!has_playable_rect_) return whole;
    if (whole_map_if_allowed) {
        const ArmyBrain* a = get_army(army);
        if (a && a->use_whole_map()) return whole;
    }
    return {static_cast<i32>(playable_x0_), static_cast<i32>(playable_z0_),
            static_cast<i32>(playable_x1_), static_cast<i32>(playable_z1_)};
}

u8 SimState::footprint_fits_at(const blueprints::Footprint& fp, f32 wx, f32 wz) const {
    if (!terrain_) return 0;
    const OccupancyRect r = footprint_rect(fp, wx, wz);
    return footprint_fits(fp, *terrain_, occupancy_, r.x0, r.z0);
}

void SimState::notify_script_destroy(Entity& entity) {
    if (!L_ || entity.script_destroy_notified() || entity.lua_table_ref() < 0) return;
    entity.set_script_destroy_notified();
    const int top = lua_gettop(L_);
    lua_rawgeti(L_, LUA_REGISTRYINDEX, entity.lua_table_ref());
    if (lua_istable(L_, -1)) {
        lua_pushstring(L_, "OnDestroy");
        lua_gettable(L_, -2);
        if (lua_isfunction(L_, -1)) {
            lua_pushvalue(L_, -2);
            if (lua_pcall(L_, 1, 0, 0) != 0) {
                spdlog::warn("OnDestroy error: {}", lua_tostring(L_, -1));
            }
        }
    }
    lua_settop(L_, top);
}

namespace {

void call_with(lua_State* L, Entity& self, const char* method, const Entity& other) {
    if (!L || self.lua_table_ref() < 0 || other.lua_table_ref() < 0) {
        return;
    }
    const int top = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, self.lua_table_ref());
    lua_pushstring(L, method);
    lua_gettable(L, -2);
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, top + 1);
        lua_rawgeti(L, LUA_REGISTRYINDEX, other.lua_table_ref());
        if (lua_pcall(L, 2, 0, 0) != 0) {
            spdlog::warn("{} error: {}", method, lua_tostring(L, -1));
        }
    }
    lua_settop(L, top);
}

bool script_says(lua_State* L, Entity& self, const char* method) {
    if (!L || self.lua_table_ref() < 0) {
        return false;
    }
    const int top = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, self.lua_table_ref());
    lua_pushstring(L, method);
    lua_gettable(L, -2);
    bool yes = false;
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, top + 1);
        lua_pushvalue(L, top + 1);
        if (lua_pcall(L, 2, 1, 0) == 0) {
            yes = lua_toboolean(L, -1) != 0;
        } else {
            spdlog::warn("{} error: {}", method, lua_tostring(L, -1));
        }
    }
    lua_settop(L, top);
    return yes;
}

} // namespace

void SimState::on_destroy(Entity& entity) {
    if (entity.is_unit()) {
        auto& unit = static_cast<Unit&>(entity);
        unit.release_weapon_scripts(L_);
        if (!unit.is_dying()) {
            drop_adjacency(unit);
            detach_all_units(unit, false);
        }
        const bool handed_over = unit.transferred();
        if (!handed_over && !unit.is_dying()) {
            record_unit_death(unit);
        }
        if (!handed_over) {
            const Vector3 pos = unit.position();
            const f32 scale = unit.footprint_size_x() > 0 ? unit.footprint_size_x() * 0.5f : 1.0f;
            add_death_event(pos.x, pos.y, pos.z, scale, unit.army());
        }
        if (unit.is_capturing()) {
            unit.stop_capturing(L_, entity_registry_, true);
        }
    }
    notify_script_destroy(entity);
}

void SimState::drop_adjacency(Unit& unit) {
    const std::vector<u32> neighbours(unit.adjacent_unit_ids().begin(),
                                      unit.adjacent_unit_ids().end());
    unit.clear_adjacents();
    for (const u32 id : neighbours) {
        Entity* e = entity_registry_.find(id);
        if (!e || !e->is_unit()) {
            continue;
        }
        static_cast<Unit*>(e)->remove_adjacent(unit.entity_id());
        call_with(L_, unit, "OnNotAdjacentTo", *e);
        call_with(L_, *e, "OnNotAdjacentTo", unit);
    }
}

void SimState::detach_all_units(Unit& transport, bool destroy_some) {
    std::vector<u32> cargo;
    for (const u32 id : transport.cargo_ids()) {
        Entity* e = entity_registry_.find(id);
        if (e && !e->destroyed() && e->is_unit() && !static_cast<Unit*>(e)->is_dying()) {
            cargo.push_back(id);
        }
    }
    std::vector<u32> set_down;
    for (const u32 id : cargo) {
        auto& unit = static_cast<Unit&>(*entity_registry_.find(id));
        if (destroy_some && sim_random_.range(0.0f, 1.0f) < 0.99f) {
            if (script_says(L_, unit, "CheckCanBeKilled")) {
                transport.remove_cargo(id);
                unit.set_transport_id(0);
                if (L_ && unit.lua_table_ref() >= 0) {
                    const int top = lua_gettop(L_);
                    lua_rawgeti(L_, LUA_REGISTRYINDEX, unit.lua_table_ref());
                    lua_pushstring(L_, "Kill");
                    lua_gettable(L_, -2);
                    if (lua_isfunction(L_, -1)) {
                        lua_pushvalue(L_, top + 1);
                        if (transport.lua_table_ref() >= 0) {
                            lua_rawgeti(L_, LUA_REGISTRYINDEX, transport.lua_table_ref());
                        } else {
                            lua_pushnil(L_);
                        }
                        lua_pushstring(L_, "Damage");
                        lua_pushnumber(L_, 0);
                        if (lua_pcall(L_, 4, 0, 0) != 0) {
                            spdlog::warn("Kill error: {}", lua_tostring(L_, -1));
                        }
                    }
                    lua_settop(L_, top);
                } else {
                    entity_registry_.destroy(unit);
                }
                continue;
            }
            static const CategoryName kCommand{"COMMAND"};
            if (unit.has_category(kCommand) && script_says(L_, unit, "CheckCanTakeDamage")) {
                if (L_ && unit.lua_table_ref() >= 0) {
                    const int top = lua_gettop(L_);
                    lua_rawgeti(L_, LUA_REGISTRYINDEX, unit.lua_table_ref());
                    lua_pushstring(L_, "OnDamage");
                    lua_gettable(L_, -2);
                    if (lua_isfunction(L_, -1)) {
                        lua_pushvalue(L_, top + 1);
                        if (transport.lua_table_ref() >= 0) {
                            lua_rawgeti(L_, LUA_REGISTRYINDEX, transport.lua_table_ref());
                        } else {
                            lua_pushnil(L_);
                        }
                        lua_pushnumber(L_, 10000);
                        lua_pushboolean(L_, 0);
                        lua_pushstring(L_, "Damage");
                        if (lua_pcall(L_, 5, 0, 0) != 0) {
                            spdlog::warn("OnDamage error: {}", lua_tostring(L_, -1));
                        }
                    }
                    lua_settop(L_, top);
                }
                continue;
            }
        }
        set_down.push_back(id);
    }
    transport.detach_cargo(set_down, entity_registry_, L_, terrain_.get());
    const std::vector<u32> stored = transport.stored_ids();
    for (const u32 id : stored) {
        Entity* e = entity_registry_.find(id);
        if (!e || e->destroyed() || !e->is_unit()) {
            continue;
        }
        auto& unit = static_cast<Unit&>(*e);
        transport.forget_stored(id);
        unit.set_transport_id(0);
        unit.call_lua_method(L_, "DestroyedOnTransport");
        if (!unit.destroyed()) {
            unit.call_lua_method(L_, "Destroy");
        }
        if (!unit.destroyed()) {
            entity_registry_.destroy(unit);
        }
    }
}

void SimState::record_unit_death(Unit& unit) {
    if (auto* brain = get_army(unit.army())) {
        brain->record_unit_lost(unit.blueprint_id(), unit.build_cost_mass(),
                                unit.build_cost_energy());
    }
    if (!unit.damage_contributions().empty()) {
        i32 killer_army = -1;
        f32 max_dmg = 0;
        for (const auto& [attacker_id, dmg] : unit.damage_contributions()) {
            if (dmg > max_dmg) {
                const Entity* attacker = entity_registry_.find(attacker_id);
                if (attacker && !attacker->destroyed()) {
                    max_dmg = dmg;
                    killer_army = attacker->army();
                }
            }
        }
        if (killer_army >= 0 && killer_army != unit.army()) {
            if (auto* killer = get_army(killer_army)) {
                killer->record_enemy_killed(unit.blueprint_id(), unit.build_cost_mass(),
                                            unit.build_cost_energy(), unit.has_category("COMMAND"));
            }
        }
    }
    unit.clear_damage_contributions();
}

void SimState::on_entity_unregistered(Entity& entity) {
    // Unregistered outside the deletion queue: its script's OnDestroy still runs, first.
    notify_script_destroy(entity);
    // Its ambient loops end with it: the audio side stops what no entity wants.
    entity.clear_ambient_sounds();

    // Its ground is free (Moho's ReleaseOccupyGround, a prop's destructor),
    // and any place it reserved to land on.
    release_ground(entity.entity_id());
    if (entity.is_unit()) static_cast<Unit&>(entity).free_landing_reservation(*this);

    // A dead structure stops blocking paths (it used to block forever).
    if (auto it = occupied_footprints_.find(entity.entity_id());
        it != occupied_footprints_.end()) {
        if (pathfinding_grid_) {
            const auto& fp = it->second;
            pathfinding_grid_->clear_obstacle(fp.x, fp.z, fp.size_x, fp.size_z);
        }
        occupied_footprints_.erase(it);
    }

    // Its manipulators and weapons go with it; their Lua tables are detached.
    if (entity.is_unit()) {
        auto& unit = static_cast<Unit&>(entity);
        unit.release_manipulators(L_);
        unit.release_weapon_scripts(L_);
        unit.end_script_task(L_); // its OnDestroy, the unit still in reach (M206w)
        // A stored unit leaves its carrier's storage; a carrier's stored
        // units go with it (Moho's ~CAiTransportImpl), destroyed once this
        // unregistration is over (M206q).
        if (unit.transport_id() != 0)
            if (Entity* c = entity_registry_.find(unit.transport_id()); c && c->is_unit())
                static_cast<Unit*>(c)->forget_stored(unit.entity_id());
        for (const u32 id : unit.stored_ids()) stored_to_destroy_.push_back(id);
    }
}

void SimState::release_script_handle(Entity& entity) {
    if (!L_) return;
    // Handles its scripts made after it was unregistered go with it too:
    // the weapon tables quietly (they never saw OnCreate, so no OnDestroy).
    if (entity.is_unit()) {
        auto& unit = static_cast<Unit&>(entity);
        unit.release_manipulators(L_);
        unit.release_weapon_scripts(L_, /*run_on_destroy=*/false);
    }
    // The Lua table outlives the C++ object: null its _c_object so methods
    // called on a stale handle see "destroyed" instead of freed memory, and
    // release the registry reference.
    const int ref = entity.lua_table_ref();
    if (ref < 0) return;
    lua_rawgeti(L_, LUA_REGISTRYINDEX, ref);
    if (lua_istable(L_, -1)) {
        lua_pushstring(L_, "_c_object");
        lua_pushlightuserdata(L_, nullptr);
        lua_rawset(L_, -3);
    }
    lua_pop(L_, 1);
    luaL_unref(L_, LUA_REGISTRYINDEX, ref);
    entity.set_lua_table_ref(LUA_NOREF);
}

void SimState::kill_cleanup() {
    std::vector<std::pair<size_t, u32>> due;
    entity_registry_.for_each_unit([&](Entity& e) {
        if (!static_cast<const Unit&>(e).needs_kill_cleanup()) {
            return;
        }
        const i32 army = e.army();
        const bool listed = army >= 0 && static_cast<size_t>(army) < armies_.size();
        due.emplace_back(listed ? static_cast<size_t>(army) : armies_.size(), e.entity_id());
    });
    std::stable_sort(due.begin(), due.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [army, id] : due) {
        if (Entity* e = entity_registry_.find(id); e && e->is_unit()) {
            static_cast<Unit*>(e)->kill_cleanup(L_);
        }
    }
}

void SimState::run_deletion_queue() {
    std::vector<u32> purged;
    while (const u32 id = entity_registry_.take_destroyed()) {
        if (Entity* e = entity_registry_.find(id)) {
            on_destroy(*e);
            purged.push_back(id);
        }
    }
    for (const u32 id : purged) {
        entity_registry_.unregister_entity(id);
    }
}

void SimState::destroy_orphaned_stored_units() {
    while (!stored_to_destroy_.empty()) {
        const u32 id = stored_to_destroy_.back();
        stored_to_destroy_.pop_back();
        Entity* e = entity_registry_.find(id);
        if (!e || e->destroyed() || !e->is_unit()) continue;
        auto* unit = static_cast<Unit*>(e);
        unit->set_transport_id(0);
        if (L_) unit->call_lua_method(L_, "Destroy");
        e = entity_registry_.find(id);
        if (e && !e->destroyed()) { // no script object (or no Destroy)
            entity_registry_.destroy(*e);
        }
    }
}

SimState::~SimState() {
    // The sim Lua state may outlive this sim; it must not keep reaching the
    // sound engine through it.
    if (L_ && sound_manager_) {
        lua_pushstring(L_, "osc_sound_manager");
        lua_pushnil(L_);
        lua_rawset(L_, LUA_REGISTRYINDEX);
    }
}

void SimState::clean_up_platoons() {
    if (!L_) return;
    // A unit leaves its platoon only as it is freed (Moho's ~Unit): a dying
    // one still counts; one destroyed last tick is gone by now.
    const auto alive = [this](u32 id) {
        const Entity* e = entity_registry_.find(id);
        return e && !e->destroyed();
    };
    // Moho's AssignedSquadsAreIdle: no unit in an assigned squad (Attack,
    // Artillery, Guard, Support or Scout), alive and not dying, has an
    // order at the head of its queue.
    const auto idle = [&](const Platoon& p) {
        for (const u32 id : p.unit_ids()) {
            const int squad = Platoon::squad_class(p.get_unit_squad(id));
            if (squad < 1 || !alive(id)) continue;
            const Entity* e = entity_registry_.find(id);
            if (!e->is_unit()) continue;
            const auto& unit = static_cast<const Unit&>(*e);
            if (!unit.is_dying() && !unit.command_queue().empty()) return false;
        }
        return true;
    };
    for (const auto& army : armies_) {
        Platoon* pool = army->find_platoon_by_name("ArmyPool");
        std::vector<Platoon*> doomed;
        for (size_t i = 0; i < army->platoon_count(); ++i) {
            Platoon* p = army->platoon_at(i);
            if (!p || p->destroyed()) continue;
            bool disband = false;
            // (Moho would disband even the pool so; the engine keeps it, as it
            // does through DisbandPlatoon: new units go to it.)
            if (p->disband_on_idle() && p != pool && idle(*p)) {
                // Its units go to the pool's unassigned squad first.
                if (pool) {
                    const std::vector<u32> ids = p->unit_ids();
                    for (const u32 id : ids) {
                        if (!alive(id)) continue;
                        p->remove_unit(id);
                        pool->add_unit(id);
                        pool->set_unit_squad(id, "Unassigned");
                        Entity* e = entity_registry_.find(id);
                        if (e->lua_table_ref() < 0 || pool->lua_table_ref() < 0) continue;
                        lua_rawgeti(L_, LUA_REGISTRYINDEX, e->lua_table_ref());
                        lua_pushstring(L_, "PlatoonHandle");
                        lua_rawgeti(L_, LUA_REGISTRYINDEX, pool->lua_table_ref());
                        lua_rawset(L_, -3);
                        lua_pop(L_, 1);
                    }
                }
                disband = true;
            }
            if (!disband && p->unique_name().empty() &&
                std::none_of(p->unit_ids().begin(), p->unit_ids().end(), alive))
                disband = true;
            if (!disband) continue;
            // Out of the army's platoons first, then each one's OnDestroy.
            army->destroy_platoon(p);
            doomed.push_back(p);
        }
        for (Platoon* p : doomed) {
            const int ref = p->lua_table_ref();
            if (ref < 0) continue;
            p->set_lua_table_ref(-2); // LUA_NOREF
            const int top = lua_gettop(L_);
            lua_rawgeti(L_, LUA_REGISTRYINDEX, ref);
            const int table = lua_gettop(L_);
            lua_pushstring(L_, "OnDestroy");
            lua_gettable(L_, table);
            if (lua_isfunction(L_, -1)) {
                lua_pushvalue(L_, table);
                if (lua_pcall(L_, 1, 0, 0) != 0) {
                    const char* err = lua_tostring(L_, -1);
                    const std::string message =
                        std::string("Platoon OnDestroy error: ") + (err ? err : "(unknown)");
                    spdlog::warn("{}", message);
                    if (test_status::count_lua_failures()) test_status::record_failure(message);
                }
            }
            lua_settop(L_, top);
            // Scripts still holding it see a platoon that no longer exists.
            lua_rawgeti(L_, LUA_REGISTRYINDEX, ref);
            lua_pushstring(L_, "_c_object");
            lua_pushnil(L_);
            lua_rawset(L_, -3);
            lua_settop(L_, top);
            luaL_unref(L_, LUA_REGISTRYINDEX, ref);
        }
    }
}

Entity* SimState::attach_parent(const Entity& e) const {
    u32 id = e.parent_entity_id();
    if (e.is_collision_beam()) {
        id = e.beam_launcher_id();
    } else if (e.is_unit() && static_cast<const Unit&>(e).transport_id() != 0) {
        id = static_cast<const Unit&>(e).transport_id();
    }
    return id != 0 ? entity_registry_.find(id) : nullptr;
}

void SimState::follow_parent(Entity& e, const Entity& parent) {
    if (e.is_unit() && static_cast<const Unit&>(e).transport_id() != 0) {
        if (parent.is_unit()) {
            static_cast<Unit&>(e).hang_from(static_cast<const Unit&>(parent));
        }
        return;
    }
    // Moho's CalculateAttachedTransform (M211k), the child's own bone taken as its origin; another
    // entity's bone is on its committed transform (Entity::GetBoneWorldTransform).
    const BonePose bone =
        parent.is_unit() ? static_cast<const Unit&>(parent).bone_world_transform(e.parent_bone())
                         : BonePose{parent.position(), parent.orientation()};
    const BonePose at = pose_compose(bone, {e.parent_offset(), Quaternion{}});
    e.set_position(at.position);
    e.set_orientation(at.rotation);
    // Attaching is a jump, and so is following a parent that jumped since: the renderer pops it.
    if (e.followed_parent_snap() != parent.snap_serial()) {
        e.note_snap();
        e.set_followed_parent_snap(parent.snap_serial());
    }
}

void SimState::refollow_attached() {
    for (const Followed& f : followed_) {
        Entity* e = entity_registry_.find(f.id);
        const Entity* parent = entity_registry_.find(f.parent);
        if (!e || !parent || attach_parent(*e) != parent) {
            continue;
        }
        const Vector3& p = parent->pending_position();
        const Quaternion& q = parent->pending_orientation();
        if (p.x == f.at.x && p.y == f.at.y && p.z == f.at.z && q.x == f.facing.x &&
            q.y == f.facing.y && q.z == f.facing.z && q.w == f.facing.w) {
            continue;
        }
        if (e->is_collision_beam()) {
            repose_collision_beam(entity_registry_, *e);
        } else {
            follow_parent(*e, *parent);
        }
    }
}

void SimState::crash_impact(Unit& unit) {
    if (unit.destroyed()) {
        return;
    }
    const u32 id = unit.entity_id();
    if (!L_ || unit.lua_table_ref() < 0) {
        entity_registry_.destroy(unit);
        return;
    }
    const Vector3 p = unit.pending_position();
    const bool water = terrain_ && terrain_->has_water() &&
                       terrain_->get_terrain_height(p.x, p.z) < terrain_->water_elevation();
    // On the ground it is a land unit now: its wreck follows the
    // blueprint's WreckageLayers.Land.
    if (!water) {
        unit.set_layer_with_callback("Land", L_);
    }
    Entity* e = entity_registry_.find(id);
    if (!e || e->destroyed() || e->lua_table_ref() < 0) {
        return;
    }
    const int top = lua_gettop(L_);
    lua_rawgeti(L_, LUA_REGISTRYINDEX, e->lua_table_ref());
    lua_pushstring(L_, "OnImpact");
    lua_gettable(L_, -2);
    bool handled = false;
    if (lua_isfunction(L_, -1)) {
        lua_pushvalue(L_, top + 1);
        lua_pushstring(L_, water ? "Water" : "Terrain");
        lua_pushnil(L_);
        if (lua_pcall(L_, 3, 0, 0) == 0) {
            handled = true;
        } else {
            const char* err = lua_tostring(L_, -1);
            const std::string message = std::string("OnImpact error: ") + (err ? err : "(unknown)");
            spdlog::warn("{}", message);
            if (test_status::count_lua_failures()) {
                test_status::record_failure(message);
            }
        }
    }
    lua_settop(L_, top);
    // Nothing will play this death out (no OnImpact, or it broke):
    // finish it, as Kill does when OnKilled fails, rather than leave
    // a dead unit lying there forever.
    e = entity_registry_.find(id);
    if (handled || !e || e->destroyed()) {
        return;
    }
    if (e->lua_table_ref() >= 0) {
        lua_rawgeti(L_, LUA_REGISTRYINDEX, e->lua_table_ref());
        lua_pushstring(L_, "Destroy");
        lua_gettable(L_, -2);
        if (lua_isfunction(L_, -1)) {
            lua_pushvalue(L_, top + 1);
            if (lua_pcall(L_, 1, 0, 0) != 0) {
                spdlog::warn("Destroy error: {}", lua_tostring(L_, -1));
            }
        }
        lua_settop(L_, top);
    }
    e = entity_registry_.find(id);
    if (e && !e->destroyed()) {
        entity_registry_.destroy(*e);
    }
}

Entity::Walk SimState::step_entity(Entity& e, bool attached, SimContext& ctx) {
    if (e.is_unit()) {
        auto& unit = static_cast<Unit&>(e);
        collide_with_props(unit, entity_registry_, L_, tick_count());
        unit.update(SECONDS_PER_TICK, ctx);
        unit.note_queue_head();
        return Entity::Walk::Stepping;
    }
    if (e.is_projectile()) {
        static_cast<Projectile&>(e).update(SECONDS_PER_TICK, entity_registry_, L_, terrain_.get(),
                                           tick_count());
        return Entity::Walk::Stepping;
    }
    if (e.is_collision_beam()) {
        return step_collision_beam(*this, L_, e) ? Entity::Walk::Stepping : Entity::Walk::Done;
    }
    // Moho's Entity::MotionTick.
    if (attached) {
        return Entity::Walk::Stepping;
    }
    if (!e.is_prop() || !static_cast<const Prop&>(e).has_motor()) {
        return Entity::Walk::Parked;
    }
    auto& prop = static_cast<Prop&>(e);
    prop.step_fall(terrain_.get());
    if (prop.sink_rate != 0) {
        Vector3 p = prop.pending_position();
        p.y += prop.sink_rate * static_cast<f32>(SECONDS_PER_TICK);
        prop.set_position(p);
    }
    return Entity::Walk::Stepping;
}

VictoryMode parse_victory_mode(const std::string& value) {
    std::string v;
    v.reserve(value.size());
    for (unsigned char c : value) v.push_back(static_cast<char>(std::tolower(c)));

    // Canonical FA option keys plus friendly aliases (lobby label text).
    if (v == "sandbox" || v == "none") return VictoryMode::Sandbox;
    if (v == "domination" || v == "supremacy" || v == "dominance")
        return VictoryMode::Domination;
    if (v == "eradication" || v == "annihilation")
        return VictoryMode::Eradication;
    // decapitation (a FAF condition) is ACU-kill, like demoralization.
    if (v == "demoralization" || v == "assassination" || v == "decapitation")
        return VictoryMode::Demoralization;
    return VictoryMode::Demoralization; // FA default
}

void SimState::set_victory_condition(std::string mode) {
    std::transform(mode.begin(), mode.end(), mode.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    victory_mode_ = parse_victory_mode(mode);
    victory_condition_ = std::move(mode);
}

ShareMode parse_share_mode(const std::string& value) {
    std::string v;
    v.reserve(value.size());
    for (unsigned char c : value) v.push_back(static_cast<char>(std::tolower(c)));

    if (v == "fullshare" || v == "full") return ShareMode::FullShare;
    if (v == "civiliandeserter") return ShareMode::CivilianDeserter;
    if (v == "partialshare") return ShareMode::PartialShare;
    if (v == "transfertokiller") return ShareMode::TransferToKiller;
    if (v == "defectors") return ShareMode::Defectors;
    return ShareMode::ShareUntilDeath; // FA default
}

void SimState::set_share_condition(std::string mode) {
    std::transform(mode.begin(), mode.end(), mode.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    share_mode_ = parse_share_mode(mode);
    share_condition_ = std::move(mode);
}

FogMode parse_fog_mode(const std::string& value) {
    std::string v;
    v.reserve(value.size());
    for (unsigned char c : value) v.push_back(static_cast<char>(std::tolower(c)));
    if (v == "none" || v == "off") return FogMode::None;
    return FogMode::Explored; // FA default (covers "explored")
}

void SimState::set_fog_of_war(const std::string& mode) {
    fog_mode_ = parse_fog_mode(mode);
    // Without fog there are no sight grids (and with it, they're needed)
    if (intel_grids_ && intel_grids_->fog_of_war() != (fog_mode_ != FogMode::None))
        build_intel_grids();
}

void SimState::set_no_rush(f32 seconds, f32 radius) {
    no_rush_seconds_ = seconds > 0.0f ? seconds : 0.0f;
    if (radius > 0.0f) no_rush_radius_ = radius;
}

Vector3 SimState::clamp_to_playable(const Vector3& pos, i32 army) const {
    const ArmyBrain* brain = army >= 0 ? army_at(static_cast<size_t>(army)) : nullptr;
    if (!brain || !brain->use_whole_map()) return clamp_to_playable(pos);
    if (!terrain_) return pos;
    Vector3 clamped = pos;
    const auto width = static_cast<f32>(terrain_->map_width());
    const auto height = static_cast<f32>(terrain_->map_height());
    clamped.x = std::clamp(clamped.x, 0.0f, width);
    clamped.z = std::clamp(clamped.z, 0.0f, height);
    return clamped;
}

bool SimState::is_valid_teleport_destination(
    const Unit& unit, const Vector3& destination) const {
    f32 half_x = std::max(unit.footprint_size_x(), 1.0f) * 0.5f;
    f32 half_z = std::max(unit.footprint_size_z(), 1.0f) * 0.5f;

    if (has_playable_rect_) {
        if (destination.x - half_x < playable_x0_ ||
            destination.x + half_x > playable_x1_ ||
            destination.z - half_z < playable_z0_ ||
            destination.z + half_z > playable_z1_) {
            return false;
        }
    }

    if (pathfinding_grid_) {
        u32 gx0, gz0, gx1, gz1;
        pathfinding_grid_->world_to_grid(destination.x - half_x,
                                         destination.z - half_z, gx0, gz0);
        pathfinding_grid_->world_to_grid(destination.x + half_x,
                                         destination.z + half_z, gx1, gz1);
        for (u32 gz = gz0; gz <= gz1; ++gz) {
            for (u32 gx = gx0; gx <= gx1; ++gx) {
                if (!pathfinding_grid_->is_passable_for(gx, gz,
                                                        unit.layer())) {
                    return false;
                }
            }
        }
    }

    auto nearby = entity_registry_.collect_in_rect(
        destination.x - half_x, destination.z - half_z,
        destination.x + half_x, destination.z + half_z);
    for (u32 id : nearby) {
        if (id == unit.entity_id()) continue;
        auto* entity = entity_registry_.find(id);
        if (!entity || entity->destroyed() || !entity->is_unit()) continue;
        auto* other = static_cast<const Unit*>(entity);
        f32 other_half_x = std::max(other->footprint_size_x(), 1.0f) * 0.5f;
        f32 other_half_z = std::max(other->footprint_size_z(), 1.0f) * 0.5f;
        const auto& other_pos = other->position();
        bool overlaps_x = std::abs(destination.x - other_pos.x) <
                          (half_x + other_half_x);
        bool overlaps_z = std::abs(destination.z - other_pos.z) <
                          (half_z + other_half_z);
        if (overlaps_x && overlaps_z) return false;
    }

    return true;
}

void SimState::set_terrain(std::unique_ptr<map::Terrain> terrain) {
    path_tables_.reset(); // they read the old map (build_pathfinding_grid makes new)
    terrain_ = std::move(terrain);
    // One cell of the occupation grid a map cell, empty (the map's
    // structures and props claim theirs as they are made).
    occupancy_ =
        terrain_ ? OccupancyGrid(terrain_->map_width(), terrain_->map_height()) : OccupancyGrid();
    ground_occupants_.clear();
}

bool SimState::flatten_map_rect(i32 x, i32 z, i32 size_x, i32 size_z, f32 elevation) {
    if (!terrain_) {
        return false;
    }
    const auto flat = terrain_->flatten(x, z, size_x, size_z, elevation);
    if (!flat) {
        return false;
    }
    const i32 lo_x = std::max(0, x - 1);
    const i32 lo_z = std::max(0, z - 1);
    if (path_tables_) {
        path_tables_->dirty({lo_x, lo_z, flat->x1, flat->z1});
    }
    // The default pathing's cells there are classed afresh (structures
    // stay marked on them).
    if (pathfinding_grid_) {
        pathfinding_grid_->refresh_terrain(terrain_->heightmap(), flat->x0, flat->z0, flat->x1,
                                           flat->z1);
    }
    const f32 mid_x = static_cast<f32>(lo_x + flat->x1) * 0.5f;
    const f32 mid_z = static_cast<f32>(lo_z + flat->z1) * 0.5f;
    const auto reach_x = static_cast<f32>(flat->x1 - lo_x);
    const auto reach_z = static_cast<f32>(flat->z1 - lo_z);
    entity_registry_.for_each_unit([&](Entity& e) {
        auto& unit = static_cast<Unit&>(e);
        if (unit.destroyed() || unit.is_dying() || !unit.is_mobile()) {
            return;
        }
        if (unit.layer() != "Land" && unit.layer() != "Seabed") {
            return;
        }
        Vector3 p = unit.pending_position();
        if (std::abs(p.x - mid_x) > reach_x || std::abs(p.z - mid_z) > reach_z) {
            return;
        }
        p.y = unit.ground_y(terrain_.get(), p.x, p.z);
        unit.set_position(p);
    });
    return true;
}

void SimState::set_sound_manager(audio::SoundManager* mgr) {
    sound_manager_ = mgr;
}

void SimState::set_bone_cache(std::unique_ptr<BoneCache> cache) {
    bone_cache_ = std::move(cache);
}

void SimState::set_anim_cache(std::unique_ptr<AnimCache> cache) {
    anim_cache_ = std::move(cache);
}

void SimState::build_pathfinding_grid() {
    if (!terrain_) return;
    // Reset pathfinder first — it holds a reference to the old grid
    pathfinder_.reset();
    pathfinding_grid_ = std::make_unique<map::PathfindingGrid>(
        terrain_->heightmap(), terrain_->water_elevation(),
        terrain_->has_water());
    pathfinder_ = std::make_unique<map::Pathfinder>(*pathfinding_grid_);
    spdlog::info("Built pathfinding grid: {}x{} cells (cell_size={})",
                 pathfinding_grid_->grid_width(),
                 pathfinding_grid_->grid_height(),
                 pathfinding_grid_->cell_size());
    reset_path_tables();
}

void SimState::reset_path_tables() {
    path_tables_.reset();
    if (!terrain_ || !blueprint_store_ || blueprint_store_->footprint_classes().empty()) return;
    path_tables_ =
        std::make_unique<PathTables>(blueprint_store_->footprint_classes(), *terrain_, occupancy_);
}

ArmyBrain& SimState::add_army(const std::string& name,
                               const std::string& nickname) {
    auto brain = std::make_unique<ArmyBrain>();
    brain->set_index(static_cast<i32>(armies_.size()));
    brain->set_name(name);
    brain->set_nickname(nickname);
    if (name.find("CIVILIAN") != std::string::npos ||
        name.find("NEUTRAL") != std::string::npos) {
        brain->set_civilian(true);
    }
    armies_.push_back(std::move(brain));
    return *armies_.back();
}

ArmyBrain* SimState::get_army(i32 index) {
    if (index < 0 || index >= static_cast<i32>(armies_.size()))
        return nullptr;
    return armies_[index].get();
}

ArmyBrain* SimState::get_army_by_name(const std::string& name) {
    for (auto& brain : armies_) {
        if (brain->name() == name) return brain.get();
    }
    return nullptr;
}

void SimState::set_alliance(i32 army1, i32 army2, Alliance alliance) {
    if (auto* a1 = get_army(army1)) a1->set_alliance(army2, alliance);
    if (auto* a2 = get_army(army2)) a2->set_alliance(army1, alliance);
}

namespace {

/// The armies as the decals' sight rules ask about them (M212c): "can
/// detect" is line of sight now over some part of the decal's bounds, the
/// army's or an ally's (M215g: by the decal's height, under the water the
/// water grid).
class SimDecalArmies final : public DecalArmies {
public:
    explicit SimDecalArmies(const SimState& sim) : sim_(sim) {}
    size_t count() const override { return sim_.army_count(); }
    bool exists(size_t index) const override { return sim_.army_at(index) != nullptr; }
    bool civilian(size_t index) const override {
        const ArmyBrain* brain = sim_.army_at(index);
        return brain && brain->is_civilian();
    }
    bool allied(size_t army, size_t other) const override {
        return sim_.is_ally(static_cast<i32>(army), static_cast<i32>(other));
    }
    bool detects(size_t observer, const DecalBounds& rect, f32 y) const override {
        return (sim_.recon_in(rect.min_x, rect.min_z, rect.max_x, rect.max_z, y,
                              static_cast<u32>(observer)) &
                SimState::kReconLOS) != 0;
    }

private:
    const SimState& sim_;
};

} // namespace

u32 SimState::decal_sight(const DecalSpec& spec) const {
    return decal_sight_at_creation(spec, SimDecalArmies(*this));
}

void SimState::update_decal_sight() {
    const SimDecalArmies armies(*this);
    const u32 lookers = decal_sight_lookers(armies);
    size_t kept = 0;
    for (const u32 id : unsettled_decals_) {
        IEffect* fx = effect_registry_.find(id);
        if (!fx || !fx->decal()) continue; // gone
        fx->set_seen_by(decal_sight_on_tick(*fx->decal(), fx->seen_by(), fx->created_tick(),
                                            tick_count_, armies));
        // Kept while the next tick's look can still change it.
        if (!decal_sight_settled(*fx->decal(), fx->seen_by(), fx->created_tick(), tick_count_ + 1,
                                 lookers))
            unsettled_decals_[kept++] = id;
    }
    unsettled_decals_.resize(kept);
}

bool SimState::is_ally(i32 army1, i32 army2) const {
    if (army1 < 0 || army1 >= static_cast<i32>(armies_.size())) return false;
    return armies_[army1]->is_ally(army2);
}

bool SimState::is_enemy(i32 army1, i32 army2) const {
    if (army1 < 0 || army1 >= static_cast<i32>(armies_.size())) return false;
    return armies_[army1]->is_enemy(army2);
}

bool SimState::is_neutral(i32 army1, i32 army2) const {
    if (army1 < 0 || army1 >= static_cast<i32>(armies_.size())) return false;
    return armies_[army1]->is_neutral(army2);
}

void SimState::build_intel_grids() {
    if (!terrain_) return;
    const auto armies =
        static_cast<u32>(std::min(army_count(), static_cast<size_t>(MAX_VIS_ARMIES)));
    intel_grids_ = std::make_unique<map::IntelGrids>(terrain_->map_width(), terrain_->map_height(),
                                                     armies, fog_mode_ != FogMode::None);
    // What is painted stays painted, where it was (Moho rebuilds its grids
    // from its handles so after a load).
    for (const auto& [id, rec] : painted_intel_)
        for (size_t s = 0; s < kIntelSources; ++s)
            if (const IntelHandle& h = rec.handles[s]; h.enabled)
                paint_intel(rec.army, static_cast<IntelSource>(s), h.pos, h.radius, 1);
    for (const TempVision& tv : temp_visions_)
        if (tv.painted)
            paint_intel(static_cast<i32>(tv.army), IntelSource::Vision, {tv.x, 0, tv.z},
                        static_cast<u32>(std::max(0.0f, tv.radius)), 1);
}

void SimState::build_spatial_grid() {
    if (!terrain_) return;
    entity_registry_.init_spatial_grid(
        terrain_->map_width(), terrain_->map_height());
}

// --- Blip cache helpers ---

const BlipSnapshot* SimState::get_blip_snapshot(u32 entity_id,
                                                 u32 army) const {
    auto it = blip_cache_.find(entity_id);
    if (it == blip_cache_.end()) return nullptr;
    if (army >= MAX_VIS_ARMIES) return nullptr;
    auto& snap = it->second[army];
    // A snapshot is valid if entity_army has been set (>= 0)
    return snap.entity_army >= 0 ? &snap : nullptr;
}

u32 SimState::schedule_command(u32 source, const std::vector<u32>& unit_ids,
                               const UnitCommand& command, bool clear_existing) {
    const u32 exec_beat = beat_count_ + 1 + command_delay_;
    ScheduledCommand sc;
    sc.exec_beat = exec_beat;
    sc.source = source;
    sc.command = command;
    sc.unit_ids = unit_ids;
    sc.clear_existing = clear_existing;
    command_scheduler_.submit(std::move(sc));
    return exec_beat;
}

u32 SimState::schedule_callback(u32 source, SimCallbackEntry callback) {
    ScheduledCommand sc;
    sc.exec_beat = beat_count_ + 1 + command_delay_;
    sc.source = source;
    sc.callback = std::move(callback);
    const u32 exec_beat = sc.exec_beat;
    command_scheduler_.submit(std::move(sc));
    return exec_beat;
}

void SimState::submit_callback(SimCallbackEntry callback) {
    if (playback_) return; // a replay plays only what it recorded
    if (local_callback_sink_) local_callback_sink_(std::move(callback));
    else schedule_callback(0, std::move(callback));
}

namespace {

/// A silo build order (IssueSiloBuildNuke/Tactical) goes to the unit's silo,
/// which ignores it without a weapon to build for; its command queue never
/// sees it. True if `cmd` was one.
bool apply_silo_build(Unit& unit, const UnitCommand& cmd) {
    if (cmd.type != CommandType::SiloBuildNuke && cmd.type != CommandType::SiloBuildTactical)
        return false;
    const bool nuke = cmd.type == CommandType::SiloBuildNuke;
    if (unit.silo_weapon(nuke)) unit.order_silo_build(nuke);
    return true;
}

} // namespace

u32 SimState::route_command(const std::vector<u32>& unit_ids, const UnitCommand& command,
                            bool clear_existing) {
    // A player's order is a command, applied in a beat as Moho applies it:
    // under a network session it is broadcast and every peer schedules it
    // for the same beat; in single-player it is scheduled for the next beat.
    // Either way a replay can record it.
    if (human_input_active_) {
        if (playback_) return 0; // a replay plays only what it recorded
        UnitCommand issued = command;
        issued.command_id = 0;
        if (!issued.factory && issued.type != CommandType::Stop) {
            issued.command_id = next_player_command_id();
        }
        if (local_command_sink_) {
            local_command_sink_(unit_ids, issued, clear_existing);
        } else {
            schedule_command(0, unit_ids, issued, clear_existing);
        }
        return 0;
    }
    // An AI or script order, issued inside a tick: apply now. (AI runs
    // identically on every client, so its orders stay in sync without being
    // sent over the wire.)
    if (command.factory) {
        apply_factory_command(unit_ids, command, clear_existing);
        return 0;
    }
    // It goes into queues as one command with an id of its own, from the
    // sim's counter (so every peer numbers it alike), as Moho's issue
    // returns the one CUnitCommand its units share.
    UnitCommand issued = command;
    if (issued.type != CommandType::Stop && issued.command_id == 0)
        issued.command_id = next_command_id();
    pick_target_point(issued);
    bool queued = false;
    for (const auto& [uid, cmd] : expand_group_command(unit_ids, issued)) {
        auto* e = entity_registry_.find(uid);
        if (!e || e->destroyed() || !e->is_unit()) continue;
        auto* unit = static_cast<Unit*>(e);
        if (cmd.type == CommandType::Stop && clear_existing) {
            stop_unit(*unit);
        } else if (!apply_silo_build(*unit, cmd) && takes_command(*unit, cmd, clear_existing)) {
            if (clear_existing) {
                SimContext ctx = sim_context();
                unit->clear_commands(ctx);
                if (unit->destroyed()) {
                    continue;
                }
            }
            unit->push_command(cmd, false);
            queued = true;
        }
    }
    return queued ? issued.command_id : 0;
}

namespace {

bool boards(const EntityRegistry& registry, const Unit& unit, u32 target_id) {
    const Entity* e = target_id ? registry.find(target_id) : nullptr;
    if (!e || !e->is_unit()) {
        return true;
    }
    const auto& target = static_cast<const Unit&>(*e);
    if ((&target != &unit && !unit.has_command_cap("RULEUCC_CallTransport")) ||
        target.layer() == "Seabed") {
        return false;
    }
    if (target.has_category("FERRYBEACON") ||
        (target.has_category("FACTORY") && !target.has_category("AIRSTAGINGPLATFORM") &&
         !target.has_category("TELEPORTATION"))) {
        return unit.is_mobile() && !unit.has_category("TRANSPORTATION");
    }
    if (target.destroyed() || target.is_dying() || target.is_being_built() ||
        (!target.has_command_cap("RULEUCC_Transport") &&
         !target.has_category("PODSTAGINGPLATFORM"))) {
        return false;
    }
    if (&target == &unit) {
        return true;
    }
    if (!unit.is_mobile() || unit.has_category("AIR") != target.is_staging_platform() ||
        (unit.has_category("COMMAND") && !target.has_category("CANTRANSPORTCOMMANDER"))) {
        return false;
    }
    const TransportSlots* slots = const_cast<Unit&>(target).transport_slots();
    return !slots || slots->can_carry_class(unit.transport_class());
}

} // namespace

void SimState::pick_target_point(UnitCommand& command) {
    if (command.type != CommandType::Attack || command.target_id == 0) {
        return;
    }
    const Entity* target = entity_registry_.find(command.target_id);
    if (target && !target->destroyed() && target->is_unit()) {
        command.target_point = static_cast<const Unit*>(target)->pick_target_point(sim_random_);
    }
}

bool SimState::takes_command(const Unit& unit, const UnitCommand& command,
                             bool clear_existing) const {
    if (unit.is_being_built() && !unit.takes_orders_unfinished()) {
        return false;
    }
    if (clear_existing && command.type != CommandType::Stop && unit.has_unit_state("Enhancing")) {
        return false;
    }
    if (command.type == CommandType::Move || command.type == CommandType::Patrol ||
        command.type == CommandType::AggressiveMove) {
        return unit.motion_type() != "RULEUMT_None";
    }
    if (command.type == CommandType::BuildMobile || command.type == CommandType::BuildFactory) {
        return unit.has_category("FACTORY") || unit.has_category("ENGINEER") ||
               unit.has_category("NEEDMOBILEBUILD") || unit.has_category("POD");
    }
    const char* needs = command.type == CommandType::Reclaim   ? "RECLAIM"
                        : command.type == CommandType::Capture ? "CAPTURE"
                        : command.type == CommandType::Repair  ? "REPAIR"
                                                               : nullptr;
    if (needs &&
        (!unit.has_category(needs) || (unit.transport_id() != 0 && unit.has_category("POD")))) {
        return false;
    }
    if (command.type == CommandType::Attack) {
        if (!unit.is_mobile() && std::all_of(unit.weapons().begin(), unit.weapons().end(),
                                             [](const auto& w) { return w->dummy; })) {
            return false;
        }
        const Entity* target =
            command.target_id ? entity_registry_.find(command.target_id) : nullptr;
        const ArmyBrain* brain =
            unit.army() >= 0 ? army_at(static_cast<size_t>(unit.army())) : nullptr;
        return !target || target->army() < 0 || !brain || !brain->is_ally(target->army());
    }
    if (command.type == CommandType::Reclaim) {
        const Entity* target =
            command.target_id ? entity_registry_.find(command.target_id) : nullptr;
        return !target || reclaim_target_valid(*target);
    }
    if (command.type == CommandType::TransportLoad || command.type == CommandType::Dock ||
        command.type == CommandType::WaitForFerry) {
        return unit.transport_id() == 0 && boards(entity_registry_, unit, command.target_id);
    }
    if (command.type != CommandType::Guard) return true;
    // A pod, or a unit a carrier holds, guards nothing; nor does a unit
    // without the order.
    const Entity* by = unit.transport_id() ? entity_registry_.find(unit.transport_id()) : nullptr;
    if (by && by->is_unit() &&
        (static_cast<const Unit*>(by)->has_category("CARRIER") || unit.has_category("POD")))
        return false;
    if (!unit.has_command_cap("RULEUCC_Guard")) return false;
    const Entity* e = command.target_id ? entity_registry_.find(command.target_id) : nullptr;
    const Unit* target = e && e->is_unit() ? static_cast<const Unit*>(e) : nullptr;
    if (!target) return unit.is_mobile(); // a place to guard: only a mobile unit goes
    // Never itself, a factory only a factory, and never a unit guarding it:
    // scripts walk guard chains until they end (FAF's roll-off point), and a
    // unit guarding itself, which a campaign's base manager asks for, left
    // one walking forever.
    if (target == &unit) return false;
    if (unit.has_category("FACTORY") && !target->has_category("FACTORY")) return false;
    return target->guarded_unit_id() != unit.entity_id();
}

bool SimState::command_queued(u32 command_id) const {
    if (command_id == 0) return false;
    bool found = false;
    entity_registry_.for_each_unit([&](const Entity& e) {
        if (found || e.destroyed()) {
            return;
        }
        const auto& unit = static_cast<const Unit&>(e);
        const auto has_it = [&](const UnitCommand& c) { return c.command_id == command_id; };
        found = std::any_of(unit.command_queue().begin(), unit.command_queue().end(), has_it) ||
                std::any_of(unit.rally_orders().begin(), unit.rally_orders().end(), has_it);
    });
    return found;
}

u32 SimState::next_player_command_id() const {
    const auto pending = command_scheduler_.pending();
    for (u32 n = player_commands_issued_ + 1;; ++n) {
        const u32 id = ((issuing_source_ + 1) << 24) | (n & 0xFFFFFFu);
        if (std::none_of(pending.begin(), pending.end(),
                         [&](const auto& c) { return c.command.command_id == id; })) {
            return id;
        }
    }
}

namespace {

std::optional<u32> edited_command(const SimCallbackEntry& cb) {
    const auto it = cb.args.find("Command");
    const f64* id = it == cb.args.end() ? nullptr : std::get_if<f64>(&it->second);
    if (!id || !(*id >= 1.0 && *id <= 4294967295.0)) {
        return std::nullopt;
    }
    return static_cast<u32>(*id);
}

} // namespace

std::map<u32, SimState::QueueWithPending> SimState::queues_with_pending() const {
    std::map<u32, QueueWithPending> queues;
    const auto live_unit = [&](u32 id) -> const Unit* {
        const Entity* e = entity_registry_.find(id);
        if (!e || e->destroyed() || !e->is_unit()) {
            return nullptr;
        }
        return static_cast<const Unit*>(e);
    };
    const auto queue_of = [&](u32 id, const Unit& unit) -> QueueWithPending& {
        auto [it, fresh] = queues.try_emplace(id);
        if (fresh) {
            it->second.orders.assign(unit.command_queue().begin(), unit.command_queue().end());
            it->second.kept_from_queue = it->second.orders.size();
        }
        return it->second;
    };
    const auto holds = [&](u32 id, const Unit& unit, u32 command) {
        const auto it = queues.find(id);
        const auto has = [&](const UnitCommand& c) { return c.command_id == command; };
        if (it != queues.end()) {
            return std::any_of(it->second.orders.begin(), it->second.orders.end(), has);
        }
        return std::any_of(unit.command_queue().begin(), unit.command_queue().end(), has);
    };
    for (const auto& scheduled : command_scheduler_.pending()) {
        if (scheduled.callback) {
            const SimCallbackEntry& cb = *scheduled.callback;
            const bool remove = cb.func_name == kRemoveCommandCallback;
            if (!remove && cb.func_name != kSetCommandTargetCallback) {
                continue;
            }
            const auto command = edited_command(cb);
            if (!command) {
                continue;
            }
            std::vector<CommandToRetarget> orders;
            for (const u32 id : cb.unit_ids) {
                const Unit* unit = live_unit(id);
                if (!unit || !holds(id, *unit, *command)) {
                    continue;
                }
                QueueWithPending& queue = queue_of(id, *unit);
                for (size_t i = 0; i < queue.orders.size(); ++i) {
                    const UnitCommand& c = queue.orders[i];
                    if (c.command_id != *command || (!remove && (c.from_patrol || c.from_guard))) {
                        continue;
                    }
                    if (remove) {
                        queue.orders.erase(queue.orders.begin() + static_cast<std::ptrdiff_t>(i));
                        if (i < queue.kept_from_queue) {
                            --queue.kept_from_queue;
                        }
                        break;
                    }
                    orders.push_back({unit, &queue.orders[i], i == 0 && queue.kept_from_queue > 0});
                }
            }
            if (!remove) {
                retarget_command(cb, orders, std::nullopt);
            }
            continue;
        }
        if (scheduled.command.factory) {
            continue;
        }
        const UnitCommand& cmd = scheduled.command;
        for (const u32 id : scheduled.unit_ids) {
            const Unit* unit = live_unit(id);
            if (!unit) {
                continue;
            }
            QueueWithPending& queue = queue_of(id, *unit);
            if (cmd.type == CommandType::Stop && scheduled.clear_existing) {
                queue.orders.clear();
                queue.kept_from_queue = 0;
                continue;
            }
            if (cmd.type == CommandType::SiloBuildNuke ||
                cmd.type == CommandType::SiloBuildTactical ||
                !takes_command(*unit, cmd, scheduled.clear_existing)) {
                continue;
            }
            if (scheduled.clear_existing) {
                queue.orders.clear();
                queue.kept_from_queue = 0;
            }
            queue.orders.push_back(cmd);
        }
    }
    return queues;
}

void SimState::route_player_command(const std::vector<u32>& unit_ids, const UnitCommand& command,
                                    bool clear_existing) {
    // Moho's UI splits these three by the RALLYPOINT category
    // (SplitSelectionByRallyPointCategory in its move, patrol and
    // call-transport arms); any other order goes to the selection as it is.
    const bool rally_kind = command.type == CommandType::Move ||
                            command.type == CommandType::Patrol ||
                            command.type == CommandType::TransportLoad;
    std::vector<u32> rally, others;
    for (const u32 id : unit_ids) {
        const Entity* e = entity_registry_.find(id);
        const bool rally_point = rally_kind && e && e->is_unit() &&
                                 static_cast<const Unit*>(e)->has_category("RALLYPOINT");
        (rally_point ? rally : others).push_back(id);
    }
    if (!others.empty()) route_command(others, command, clear_existing);
    if (!rally.empty()) {
        UnitCommand factory = command;
        factory.factory = true;
        factory.formation.clear(); // a rally point, not a place in a formation
        route_command(rally, factory, clear_existing);
    }
}

void SimState::apply_factory_command(const std::vector<u32>& unit_ids, const UnitCommand& command,
                                     bool clear_existing) {
    UnitCommand rally = command;
    rally.factory = false;
    const bool no_rush = no_rush_active();
    for (const u32 id : unit_ids) {
        Entity* e = entity_registry_.find(id);
        if (!e || e->destroyed() || !e->is_unit()) continue;
        auto& unit = static_cast<Unit&>(*e);
        if (!unit.keeps_rally_orders()) continue;
        // Moho passes over a factory whose rally point No Rush forbids.
        if (no_rush) {
            const Vector3 in_zone = clamp_to_no_rush(unit, rally.target_pos);
            if (in_zone.x != rally.target_pos.x || in_zone.z != rally.target_pos.z) continue;
        }
        if (clear_existing) unit.clear_rally_orders();
        unit.add_rally_order(rally);
    }
}

std::vector<std::pair<u32, UnitCommand>>
SimState::expand_group_command(const std::vector<u32>& unit_ids, const UnitCommand& command) const {
    std::vector<std::pair<u32, UnitCommand>> out;
    if (!command.formation.empty() &&
        (command.type == CommandType::Move || command.type == CommandType::AggressiveMove)) {
        const auto slots = plan_formation(
            L_, entity_registry_, terrain_.get(), unit_ids, command.formation, command.target_pos,
            command.has_facing ? std::optional<f32>(command.facing) : std::nullopt);
        if (!slots.empty()) {
            // The formation keeps its slowest surface unit's pace.
            f32 pace = 0;
            for (const auto& slot : slots) {
                const auto* u = static_cast<const Unit*>(entity_registry_.find(slot.unit_id));
                if (!u || u->is_air_unit() || u->effective_speed() <= 0) continue;
                pace = pace > 0 ? std::min(pace, u->effective_speed()) : u->effective_speed();
            }
            for (const auto& slot : slots) {
                UnitCommand cmd = command;
                cmd.target_pos = slot.position;
                cmd.formation.clear();
                cmd.speed_cap = command.form_move ? pace : 0;
                cmd.formed = command.form_move;
                out.emplace_back(slot.unit_id, std::move(cmd));
            }
            return out;
        }
    }
    // A FormAttack's formation is centred on its target, a unit or a point
    // (Moho's attack task sets the formation's coords there); each unit keeps
    // its slot as an offset from it, so the slots follow a target that moves.
    if (command.form_attack && !command.formation.empty() && command.type == CommandType::Attack) {
        const auto slots = plan_formation(
            L_, entity_registry_, terrain_.get(), unit_ids, command.formation, command.target_pos,
            command.has_facing ? std::optional<f32>(command.facing) : std::nullopt);
        if (!slots.empty()) {
            for (const auto& slot : slots) {
                UnitCommand cmd = command;
                cmd.has_slot = true;
                cmd.slot_offset = {slot.position.x - command.target_pos.x, 0.0f,
                                   slot.position.z - command.target_pos.z};
                out.emplace_back(slot.unit_id, std::move(cmd));
            }
            return out;
        }
    }
    for (const u32 id : unit_ids) out.emplace_back(id, command);
    return out;
}

void SimState::set_recording(bool on) {
    recording_ = on;
    if (!on) return;
    const bool had_setup = recorded_replay_.has_setup;
    GameSetup setup = std::move(recorded_replay_.setup);
    recorded_replay_ = Replay{};
    recorded_replay_.has_setup = had_setup;
    recorded_replay_.setup = std::move(setup);
    recorded_replay_.seed = seed_;
    recorded_replay_.build = build_id();
    recorded_replay_.command_delay = command_delay_;
    recorded_replay_.victory_condition = victory_condition_;
    recorded_replay_.final_beat = beat_count_;
    recorded_replay_.checksum_from = beat_count_ + 1;
}

void SimState::set_game_setup(GameSetup setup) {
    recorded_replay_.setup = std::move(setup);
    recorded_replay_.has_setup = true;
}

void SimState::queue_replay(const Replay& replay) {
    command_delay_ = replay.command_delay;
    pause_holds_ = replay.version >= 17;
    if (!replay.victory_condition.empty())
        set_victory_condition(replay.victory_condition);
    for (const auto& c : replay.commands) command_scheduler_.submit(c);
}

void SimState::adopt_history(const Replay& saved) {
    recorded_replay_ = saved;
    // The pathing the restored game plays with is the snapshot's; a history
    // older than replay format 16 doesn't record it, and a later save of
    // this game must.
    recorded_replay_.setup.moho_pathing = moho_pathing_;
    std::erase_if(recorded_replay_.commands,
                  [&](const ScheduledCommand& c) { return c.exec_beat > saved.final_beat; });
    recording_ = true;
    playback_ = false;
    resume_beat_ = 0;
}

void SimState::start_resume(const Replay& saved) {
    queue_replay(saved);
    const bool behind = saved.final_beat > beat_count_;
    playback_ = behind;
    resume_beat_ = behind ? saved.final_beat : 0;
}

std::shared_ptr<const ProjectileBlueprintInfo>
SimState::projectile_blueprint_info(const std::string& bp_id) {
    if (auto it = projectile_info_.find(bp_id); it != projectile_info_.end()) return it->second;
    auto info = std::make_shared<ProjectileBlueprintInfo>();
    info->categories.insert("ALLPROJECTILES");
    if (L_ && !bp_id.empty()) {
        const int top = lua_gettop(L_);
        if (blueprints::push_blueprint(L_, bp_id)) {
            const int bp = lua_gettop(L_);
            collect_blueprint_categories(L_, bp, info->categories);
            lua_pushstring(L_, "DesiredShooterCap");
            lua_rawget(L_, bp);
            if (lua_type(L_, -1) == LUA_TNUMBER && lua_tonumber(L_, -1) > 0)
                info->desired_shooter_cap = static_cast<u32>(lua_tonumber(L_, -1));
            lua_pushstring(L_, "Display");
            lua_rawget(L_, bp);
            if (lua_istable(L_, -1)) {
                const int display = lua_gettop(L_);
                lua_pushstring(L_, "CameraFollowsProjectile");
                lua_rawget(L_, display);
                info->camera_follows = lua_toboolean(L_, -1) != 0;
                lua_pushstring(L_, "CameraFollowTimeout");
                lua_rawget(L_, display);
                if (lua_type(L_, -1) == LUA_TNUMBER) {
                    info->camera_follow_timeout = static_cast<f32>(lua_tonumber(L_, -1));
                }
            }
        }
        lua_settop(L_, top);
    }
    projectile_info_.emplace(bp_id, info);
    return info;
}

void SimState::stop_unit(Unit& unit) {
    SimContext ctx = sim_context();
    unit.clear_commands(ctx);
    if (!unit.destroyed()) {
        unit.run_stop(L_);
    }
    unit.request_ui_refresh();
}

void SimState::request_pause(u32 source) {
    if (paused_by_ >= 0 || source == kEngineSource) {
        return;
    }
    i32 left = pause_timeouts(source);
    if (left == 0) {
        spdlog::info("[pause] source {} has no timeouts left", source);
        return;
    }
    if (left > 0) {
        set_pause_timeouts(source, --left);
    }
    paused_by_ = static_cast<i32>(source);
    ++pause_serial_;
    spdlog::info("[pause] source {} paused at tick {} ({} timeouts left)", source, tick_count_,
                 left);
    if (!pause_holds_) {
        paused_by_ = -1;
    }
}

void SimState::resume(u32 source) {
    if (paused_by_ >= 0 && source != kEngineSource) {
        spdlog::info("[pause] source {} resumed at tick {}", source, tick_count_);
        paused_by_ = -1;
    }
}

void SimState::single_step(u32 source) {
    if (paused_by_ >= 0 && source != kEngineSource) {
        single_step_ = true;
    }
}

void SimState::begin_command_sources() {
    command_sources_known_ = true;
    army_sources_.clear();
}

void SimState::set_command_source(i32 army, u32 source, bool valid) {
    if (!get_army(army)) return;
    if (valid) army_sources_[army].insert(source);
    else if (const auto it = army_sources_.find(army); it != army_sources_.end())
        it->second.erase(source);
}

bool SimState::army_has_command_source(i32 army, u32 source) const {
    const auto it = army_sources_.find(army);
    return it != army_sources_.end() && it->second.count(source) != 0;
}

std::vector<u32> SimState::army_command_sources(i32 army) const {
    const auto it = army_sources_.find(army);
    if (it == army_sources_.end()) return {};
    return {it->second.begin(), it->second.end()};
}

bool SimState::cheats_enabled() {
    // As Moho's, which names the source's player ("???" without one)
    if (!cheats_enabled_) {
        if (current_command_source_ == kEngineSource) spdlog::warn("??? is trying to cheat!");
        else spdlog::warn("source {} is trying to cheat!", current_command_source_);
    }
    return cheats_enabled_;
}

bool SimState::okay_to_mess_with(i32 army) {
    if (!command_sources_known_) return true;
    const ArmyBrain* brain = get_army(army);
    if (!brain) return cheats_enabled();
    if (brain->is_defeated()) return false; // out of the game
    if (current_command_source_ != kEngineSource &&
        army_has_command_source(army, current_command_source_))
        return true;
    return cheats_enabled();
}

void SimState::terminate_command_source(u32 source) {
    // Moho's Sim::OnCommandSourceTerminated resumes the pause its source took.
    if (paused_by_ == static_cast<i32>(source)) {
        paused_by_ = -1;
    }
    if (!command_sources_known_) {
        defeat_army(army_of_source(source));
        return;
    }
    spdlog::info("Command source {} terminated tick {}", source, tick_count_);
    for (auto& [army, sources] : army_sources_) {
        if (sources.erase(source) == 0 || !sources.empty()) continue;
        // Abandoned: the brain decides (retail's AbandonedByPlayer calls
        // OnDefeat; FAF's may share the army with its allies instead)
        ArmyBrain* brain = get_army(army);
        if (!brain) continue;
        spdlog::info("Army {} ({}) abandoned: no player commands it", army, brain->name());
        bool called = false;
        if (L_ && brain->lua_table_ref() >= 0) {
            const int top = lua_gettop(L_);
            lua_rawgeti(L_, LUA_REGISTRYINDEX, brain->lua_table_ref());
            lua_pushstring(L_, "AbandonedByPlayer");
            lua_gettable(L_, -2);
            if (lua_isfunction(L_, -1)) {
                called = true;
                lua_pushvalue(L_, top + 1);
                if (lua_pcall(L_, 1, 0, 0) != 0) {
                    const char* err = lua_tostring(L_, -1);
                    spdlog::warn("AbandonedByPlayer (army {}): {}", army, err ? err : "?");
                }
            }
            lua_settop(L_, top);
        }
        if (!called) defeat_army(army);
    }
}

void SimState::dispatch_due_commands() {
    PROFILE_ZONE("Sim::commands");
    const bool no_rush = no_rush_active();
    command_scheduler_.dispatch_due(beat_count_, [&](const ScheduledCommand& scheduled) {
        ScheduledCommand sc = scheduled;
        // Its source runs it, as Moho's sim plays a source's part of the
        // command stream (the engine's own commands have none).
        current_command_source_ = sc.source;
        struct SourceDone {
            u32& source;
            ~SourceDone() { source = kEngineSource; }
        } source_done{current_command_source_};
        // A player's end comes from the engine, never from a player's frame
        if (sc.callback && sc.source != kEngineSource &&
            (sc.callback->func_name == kDefeatArmyCallback ||
             sc.callback->func_name == kCommandSourceTerminatedCallback)) {
            spdlog::warn("[commands] source {} sent the engine's {}; ignored", sc.source,
                         sc.callback->func_name);
            return;
        }
        // A player's order, or UI callback, moves only the units its source
        // may command (Moho's IssueCommand and ProcessInfoPair pass over the
        // rest). A recording holds them as applied, so a replay's are
        // taken as they come.
        if (sc.source != kEngineSource && !playback_) {
            const auto commandable_only = [&](std::vector<u32>& ids) {
                const size_t named = ids.size();
                ids.erase(std::remove_if(ids.begin(), ids.end(),
                                         [&](u32 id) {
                                             const Entity* e = entity_registry_.find(id);
                                             return !e || !okay_to_mess_with(e->army());
                                         }),
                          ids.end());
                if (ids.size() != named)
                    spdlog::warn("[commands] source {} named {} units it can't command; "
                                 "ignored",
                                 sc.source, named - ids.size());
            };
            commandable_only(sc.unit_ids);
            if (sc.callback) commandable_only(sc.callback->unit_ids);
        }
        // A recording keeps what the sim applies, where it applies it: local
        // and networked commands alike, in the beat they ran.
        if (recording_) {
            recorded_replay_.commands.push_back(sc);
            recorded_replay_.commands.back().exec_beat = beat_count_;
        }
        if (sc.callback && sc.callback->func_name == kRequestPauseCallback) {
            request_pause(sc.source); // (a callback doesn't know its source)
            return;
        }
        if (sc.callback && sc.callback->func_name == kResumeCallback) {
            resume(sc.source);
            return;
        }
        if (sc.callback && sc.callback->func_name == kSingleStepCallback) {
            single_step(sc.source);
            return;
        }
        if (sc.callback) {
            run_sim_callback(*sc.callback);
            return;
        }
        UnitCommand base = sc.command;
        if (base.command_id == 0) {
            base.command_id = next_command_id();
        } else if (base.command_id >> 24 != 0) {
            player_commands_issued_ =
                std::max(player_commands_issued_, base.command_id & 0xFFFFFFu);
        }
        if (base.factory) {
            apply_factory_command(sc.unit_ids, base, sc.clear_existing);
            return;
        }
        pick_target_point(base);
        for (auto& [uid, expanded] : expand_group_command(sc.unit_ids, base)) {
            auto* e = entity_registry_.find(uid);
            if (!e || e->destroyed() || !e->is_unit()) continue;
            auto* unit = static_cast<Unit*>(e);
            if (sc.command.type == CommandType::Stop && sc.clear_existing) {
                stop_unit(*unit);
                continue;
            }
            if (apply_silo_build(*unit, sc.command)) continue;
            UnitCommand cmd = std::move(expanded);
            // A new enhancement replaces one under way, as IssueEnhancement does.
            if (cmd.type == CommandType::Enhance && unit->is_enhancing()) {
                unit->cancel_enhance(L_);
                if (unit->destroyed()) continue;
            }
            // During No Rush, clamp movement/attack goals into the unit's zone
            // so scheduler-routed orders stop cleanly at the line.
            if (no_rush && (cmd.type == CommandType::Move ||
                            cmd.type == CommandType::Attack)) {
                cmd.target_pos = clamp_to_no_rush(*unit, cmd.target_pos);
            }
            // Each selected unit independently replaces (fresh order) or
            // appends (queued/shift) — matching the Issue* bindings.
            if (!takes_command(*unit, cmd, sc.clear_existing)) continue;
            if (sc.clear_existing) {
                SimContext ctx = sim_context();
                unit->clear_commands(ctx);
                if (unit->destroyed()) {
                    continue;
                }
            }
            unit->push_command(cmd, false);
        }
    });
}

Vector3 SimState::clamp_to_no_rush(const Unit& unit, const Vector3& target) const {
    const ArmyBrain* brain = army_at(static_cast<size_t>(unit.army()));
    if (!brain) return target;
    const Vector3& c = brain->start_position();
    f32 dx = target.x - c.x;
    f32 dz = target.z - c.z;
    f32 dist_sq = dx * dx + dz * dz;
    if (dist_sq <= no_rush_radius_ * no_rush_radius_) return target;
    f32 scale = no_rush_radius_ / std::sqrt(dist_sq);
    Vector3 clamped = target;
    clamped.x = c.x + dx * scale;
    clamped.z = c.z + dz * scale;
    return clamped;
}

void SimState::enforce_no_rush() {
    if (!no_rush_active()) return;
    PROFILE_ZONE("Sim::no_rush");
    entity_registry_.for_each_unit([&](Entity& e) {
        if (e.destroyed() || !e.is_unit()) return;
        auto* unit = static_cast<Unit*>(&e);
        const ArmyBrain* brain = army_at(static_cast<size_t>(unit->army()));
        if (!brain || brain->is_civilian()) return; // wildlife/props aren't confined
        const Vector3& c = brain->start_position();
        const Vector3& p = unit->pending_position();
        f32 dx = p.x - c.x;
        f32 dz = p.z - c.z;
        f32 dist_sq = dx * dx + dz * dz;
        if (dist_sq <= no_rush_radius_ * no_rush_radius_) return;
        // Pin the unit at the no-rush boundary.
        f32 scale = no_rush_radius_ / std::sqrt(dist_sq);
        Vector3 pinned = p;
        pinned.x = c.x + dx * scale;
        pinned.z = c.z + dz * scale;
        unit->set_position(pinned);
    });
}

void SimState::tick() {
    PROFILE_ZONE("Sim::tick");
    ++beat_count_;
    if (rng_trace_) {
        const bool on = tick_count_ + 1 >= rng_trace_from_ && tick_count_ + 1 <= rng_trace_to_;
        sim_random_.set_draw_hook(on ? &SimState::trace_rng_draw : nullptr, this);
    }

    // Lua collections (M224g), from the first beat on (loading, which runs
    // no beats, collects as it did):
    // - the blueprints, loaded and modded by now, freeze: half the live heap,
    //   which each collection would mark and sweep again;
    // - the last collection's sweep runs a slice a beat;
    // - the sim collects only on its schedule (M208c). Lua's own threshold
    //   follows the heap's byte count, which isn't game state: it differs
    //   between builds (Instruction is 8 bytes on Linux, 4 on Windows) and
    //   between runs (where an address-hashed key lands decides when its
    //   table grows), and a restored snapshot's differs again. A collection
    //   it started would clear weak tables on different ticks on each peer.
    if (L_) {
        if (beat_count_ == 1) {
            if (blueprints::push_blueprints(L_)) {
                lua_freeze(L_, -1);
                lua_pop(L_, 1);
            }
            lua_setlazysweep(L_, 1);
            lua_setmanualgc(L_, 1);
        }
        lua_sweepstep(L_, LUA_SWEEP_SLICE);
    }

    // Moho's command stream applies a beat's commands as it decodes them, before ++mCurTick.
    dispatch_due_commands();
    const bool ticks = !game_over_ && (paused_by_ < 0 || single_step_);
    if (ticks) {
        single_step_ = false;
        advance_tick();
    }
    end_beat(ticks);
}

void SimState::advance_tick() {
    tick_count_++;
    game_time_ = tick_count_ * SECONDS_PER_TICK;
    // Moho's UpdatePaths: the clusters' background rebuild, before the
    // armies tick (Sim::AdvanceBeat).
    if (path_tables_) {
        PROFILE_ZONE("Sim::path_clusters");
        path_tables_->update_background(path::kBackgroundBudget);
    }
    // Moho clears the beat's resource accumulators globally before any army or task runs.
    // Paused beats never enter advance_tick, so their last report remains available.
    entity_registry_.for_each_unit([](Entity& e) {
        auto& unit = static_cast<Unit&>(e);
        if (!unit.destroyed() && !unit.is_dying()) {
            auto& economy = unit.economy();
            economy.produced_mass = 0;
            economy.produced_energy = 0;
            economy.reclaimed_mass = 0;
            economy.reclaimed_energy = 0;
        }
    });
    // Each army's tick begins by destroying its spent platoons (Moho's CArmyImpl::OnTick).
    clean_up_platoons();
    update_influence_maps();

    if (pathfinder_) {
        pathfinder_->reset_request_count();
    }

    tick_armies();
    tick_unit_orders();
    {
        PROFILE_ZONE("Sim::threads");
        thread_manager_.resume_all(tick_count_);
    }

    update_entities();
    count_held_storage();
    sweep_ferry_beacons();

    // "No Rush": pin units that strayed beyond their confinement radius.
    enforce_no_rush();
    refollow_attached();

    update_visibility();
    update_decal_sight();
    feed_influence_map();

    // --- Victory-condition enforcement (mode + team aware) ---
    update_victory();

    // A headless run has no frames, so the sim tick is its sound clock.
    if (sound_manager_ && sound_manager_->sim_clocked()) {
        PROFILE_ZONE("Sim::audio");
        sound_manager_->update(0.1f);
    }

    {
        PROFILE_ZONE("Sim::econ_events");
        tick_economy_events();
    }

    destroy_orphaned_stored_units();

    kill_cleanup();
    // Moho's AdvanceCoords, after KillCleanup: the tick's moves are committed at once.
    entity_registry_.advance_coords();
}

void SimState::end_beat(bool ticked) {
    run_deletion_queue();

    // VFX: expire timed effects (decals, splats) and garbage collect destroyed ones
    {
        PROFILE_ZONE("Sim::vfx_gc");
        effect_registry_.destroy_detached([&](u32 id) {
            const Entity* e = entity_registry_.find(id);
            return !e || e->destroyed();
        });
        effect_registry_.expire_timed(game_time_);
        effect_registry_.gc([&](IEffect& fx) {
            if (L_ && fx.lua_table_ref() >= 0)
                luaL_unref(L_, LUA_REGISTRYINDEX, fx.lua_table_ref());
        });
    }

    // A full collection of the sim's Lua state (Lua 5.0's collector is
    // stop-the-world; a threshold of 0 forces one now), on Moho's schedule.
    // Its timing is part of the game: weak tables (trash bags) lose what it
    // frees, so it must fall on the same ticks on every peer.
    if (L_ && tick_count_ % LUA_GC_PERIOD_TICKS == 0) {
        PROFILE_ZONE("Sim::lua_gc");
        lua_setgcthreshold(L_, 0);
    }

    // Entities unregistered this tick may still have been on the C++ stack
    // (destroyed from their own callbacks); only now is freeing them safe,
    // their Lua handles cut first.
    entity_registry_.collect_garbage([this](Entity& e) {
        if (e.is_prop()) {
            auto& prop = static_cast<Prop&>(e);
            if (prop.bounded_handle != -1) {
                bounded_props_.remove(prop.bounded_handle);
                prop.bounded_handle = -1;
            }
        }
        release_script_handle(e);
    });

    if (beat_observer_) {
        PROFILE_ZONE("Sim::observer");
        beat_observer_(*this);
    }
    if (checksum_trace_ || recording_) {
        const ChecksumParts& parts = beat_checksum();
        if (checksum_trace_) {
            if (!checksum_trace_header_) {
                std::string header = "# tick total";
                for (const char* name : ChecksumParts::kNames) header += fmt::format(" {}", name);
                *checksum_trace_ << header << " threads\n";
                checksum_trace_header_ = true;
            }
            std::string line = fmt::format("{} {:08x}", tick_count_, parts.total());
            for (const u64 part : parts.values()) line += fmt::format(" {:016x}", part);
            *checksum_trace_ << line << fmt::format(" {:016x}\n", parts.threads);
        }
        if (recording_) {
            recorded_replay_.final_beat = beat_count_;
            recorded_replay_.checksums.push_back(parts.total());
        }
    }
    if (entity_trace_ && ticked && tick_count_ >= entity_trace_from_ &&
        tick_count_ <= entity_trace_to_) {
        write_entity_trace();
    }
    // Death flashes and camera shakes are shown from the beat's capture;
    // the sim is done with them (events raised between beats wait for the
    // next one).
    death_events_.clear();
    camera_shake_events_.clear();
    camera_follow_events_.clear();
    sound_requests_.clear();
    intel_flush_events_.clear();
    // A loaded game has caught up: the player's orders count from here.
    if (resume_beat_ != 0 && beat_count_ >= resume_beat_) {
        resume_beat_ = 0;
        playback_ = false;
    }
    game_over_ = game_ended_;
}

void SimState::tick_army_economy(size_t index) {
    PROFILE_ZONE("Sim::economy");
    ArmyBrain& army = *armies_[index];
    request_economy_events(index);
    std::vector<ArmyBrain*> allies;
    for (const auto& other : armies_) {
        if (other.get() != &army && army.is_ally(other->index())) {
            allies.push_back(other.get());
        }
    }
    army.update_economy(entity_registry_, SECONDS_PER_TICK, allies);
    // CArmyImpl::OnTick checks the stats triggers only once mCurTick > 10.
    if (tick_count_ > 10) {
        fire_stat_triggers(army);
    }
    army.update_unit_cap_stats(entity_registry_);
}

void SimState::fire_stat_triggers(ArmyBrain& army) {
    for (const auto& name : army.take_fired_triggers()) {
        if (army.lua_table_ref() < 0) {
            continue;
        }
        lua_rawgeti(L_, LUA_REGISTRYINDEX, army.lua_table_ref());
        lua_pushstring(L_, "OnStatsTrigger");
        lua_gettable(L_, -2);
        if (lua_isfunction(L_, -1)) {
            lua_pushvalue(L_, -2);
            lua_pushstring(L_, name.c_str());
            if (lua_pcall(L_, 2, 0, 0) != 0) {
                spdlog::warn("OnStatsTrigger error: {}", lua_tostring(L_, -1));
                lua_pop(L_, 1);
            }
        } else {
            lua_pop(L_, 1);
        }
        lua_pop(L_, 1);
    }
}

void SimState::count_held_storage() {
    std::vector<std::pair<f64, f64>> held(armies_.size());
    entity_registry_.for_each_unit([&](const Entity& e) {
        const auto& unit = static_cast<const Unit&>(e);
        const i32 army = unit.army();
        if (unit.destroyed() || !unit.holds_storage() || army < 0 ||
            static_cast<size_t>(army) >= held.size()) {
            return;
        }
        held[static_cast<size_t>(army)].first += unit.economy().storage_mass;
        held[static_cast<size_t>(army)].second += unit.economy().storage_energy;
    });
    for (size_t i = 0; i < armies_.size(); ++i) {
        armies_[i]->count_storage(held[i].first, held[i].second);
    }
}

void SimState::sweep_ferry_beacons() {
    // A beacon belongs to its route's orders (Moho's CUnitCommand keeps it):
    // once no living unit has a Ferry order holding it, it goes.
    if (ferry_beacons_.empty()) return;
    std::vector<u32> held;
    entity_registry_.for_each_unit([&](const Entity& e) {
        if (e.destroyed() || !e.is_unit()) return;
        const auto& unit = static_cast<const Unit&>(e);
        if (unit.is_dying() || unit.command_queue().empty()) return;
        // Only a route's first order, at the head while the route runs, holds one.
        const UnitCommand& head = unit.command_queue().front();
        if (head.type == CommandType::Ferry && head.beacon_id != 0) held.push_back(head.beacon_id);
    });
    std::vector<u32> gone;
    for (const u32 id : ferry_beacons_)
        if (std::find(held.begin(), held.end(), id) == held.end()) gone.push_back(id);
    if (gone.empty()) return;
    ferry_beacons_.erase(std::remove_if(ferry_beacons_.begin(), ferry_beacons_.end(),
                                        [&](u32 id) {
                                            return std::find(gone.begin(), gone.end(), id) !=
                                                   gone.end();
                                        }),
                         ferry_beacons_.end());
    for (const u32 id : gone) {
        Entity* beacon = entity_registry_.find(id);
        if (!beacon || beacon->destroyed() || beacon->lua_table_ref() < 0) continue;
        const int top = lua_gettop(L_);
        lua_rawgeti(L_, LUA_REGISTRYINDEX, beacon->lua_table_ref());
        lua_pushstring(L_, "Destroy");
        lua_gettable(L_, -2);
        if (lua_isfunction(L_, -1)) {
            lua_pushvalue(L_, top + 1);
            if (lua_pcall(L_, 1, 0, 0) != 0) {
                const char* err = lua_tostring(L_, -1);
                const std::string message =
                    std::string("ferry beacon Destroy error: ") + (err ? err : "(unknown)");
                spdlog::warn("{}", message);
                if (test_status::count_lua_failures()) test_status::record_failure(message);
            }
        }
        lua_settop(L_, top);
    }
}

void SimState::request_economy_events(size_t index) {
    // Each event asks its unit's army for a tick's cost; one whose unit is
    // gone is cancelled.
    economy_events_.for_each([&](EconomyEvent& evt) {
        if (!evt.active() || evt.unit_id() == 0) {
            return;
        }
        const Entity* unit = entity_registry_.find(evt.unit_id());
        if (!unit || unit->destroyed()) {
            evt.cancel();
            return;
        }
        if (unit->army() != static_cast<i32>(index)) {
            return;
        }
        armies_[index]->add_request(evt.request());
    });
}

void SimState::step_economy_events(const Unit& unit) {
    const auto first = std::lower_bound(
        stepping_events_.begin(), stepping_events_.end(), unit.entity_id(),
        [](const std::pair<u32, EconomyEvent*>& p, u32 id) { return p.first < id; });
    for (auto it = first; it != stepping_events_.end() && it->first == unit.entity_id(); ++it) {
        EconomyEvent& evt = *it->second;
        if (!evt.active() || !evt.request().take()) {
            continue;
        }
        evt.step();
        if (evt.callback_ref() < 0 || unit.lua_table_ref() < 0) {
            continue;
        }
        const int top = lua_gettop(L_);
        lua_rawgeti(L_, LUA_REGISTRYINDEX, evt.callback_ref());
        if (lua_isfunction(L_, -1)) {
            lua_rawgeti(L_, LUA_REGISTRYINDEX, unit.lua_table_ref());
            lua_pushnumber(L_, evt.progress());
            if (lua_pcall(L_, 2, 0, 0) != 0) {
                const char* err = lua_tostring(L_, -1);
                const std::string message =
                    std::string("EconomyEvent callback error: ") + (err ? err : "(unknown)");
                spdlog::warn("{}", message);
                if (test_status::count_lua_failures()) {
                    test_status::record_failure(message);
                }
            }
        }
        lua_settop(L_, top);
        if (!unit.in_registry()) {
            return;
        }
    }
}

void SimState::tick_economy_events() {
    // An event with no unit runs on time alone.
    economy_events_.for_each([&](EconomyEvent& evt) {
        if (evt.unit_id() == 0) {
            evt.step();
        }
    });
    // Wake threads waiting on completed/cancelled events
    economy_events_.for_each([&](EconomyEvent& evt) {
        if ((evt.is_done() || evt.is_cancelled()) && evt.callback_ref() >= 0) {
            luaL_unref(L_, LUA_REGISTRYINDEX, evt.callback_ref());
            evt.set_callback_ref(LUA_NOREF);
        }
        if ((evt.is_done() || evt.is_cancelled()) && evt.has_waiting_thread())
            thread_manager_.wake(evt, tick_count_);
        // gc() frees finished events now; detach the script's handle first.
        if ((evt.is_done() || evt.is_cancelled()) && evt.lua_table_ref() >= 0) {
            lua_rawgeti(L_, LUA_REGISTRYINDEX, evt.lua_table_ref());
            if (lua_istable(L_, -1)) {
                lua_pushstring(L_, "_c_object");
                lua_pushnil(L_);
                lua_rawset(L_, -3);
            }
            lua_pop(L_, 1);
            luaL_unref(L_, LUA_REGISTRYINDEX, evt.lua_table_ref());
            evt.set_lua_table_ref(LUA_NOREF);
        }
    });
    economy_events_.gc();
}

SimContext SimState::sim_context() {
    return {entity_registry_, L_, terrain_.get(), pathfinder_.get(), pathfinding_grid_.get(), this};
}

void SimState::tick_armies() {
    PROFILE_ZONE("Sim::armies");
    std::vector<std::vector<u32>> by_army(armies_.size() + 1);
    entity_registry_.for_each_unit([&](Entity& e) {
        const i32 army = e.army();
        const bool listed = army >= 0 && static_cast<size_t>(army) < armies_.size();
        by_army[listed ? static_cast<size_t>(army) : armies_.size()].push_back(e.entity_id());
    });
    for (size_t index = 0; index < by_army.size(); ++index) {
        const std::vector<u32>& units = by_army[index];
        if (index < armies_.size()) {
            tick_army_economy(index);
        }
        if (moho_pathing_) {
            navigate_units(units);
            steer_units(*this, units);
        }
        SimContext ctx = sim_context();
        for (const u32 id : units) {
            Entity* e = entity_registry_.find(id);
            if (e && !e->destroyed() && e->is_unit() &&
                !static_cast<const Unit*>(e)->weapons().empty()) {
                static_cast<Unit*>(e)->tick_weapons(ctx);
            }
        }
        if (path_tables_ && index < armies_.size()) {
            i32 budget = path::kArmyPathBudget;
            armies_[index]->path_queue().work(*path_tables_, budget);
        }
    }
}

void SimState::navigate_units(const std::vector<u32>& ids) {
    for (const u32 id : ids) {
        Entity* e = entity_registry_.find(id);
        if (!e || e->destroyed() || !e->is_unit()) {
            continue;
        }
        auto& unit = *static_cast<Unit*>(e);
        if (unit.is_dying() || unit.is_being_built() || unit.is_air_unit() ||
            unit.transport_id() != 0) {
            continue;
        }
        unit.navigator().navigate(unit);
    }
}

void SimState::tick_unit_orders() {
    PROFILE_ZONE("Sim::orders");
    SimContext ctx = sim_context();
    std::vector<u32> ids;
    u32 done = 0;
    // CTaskStage::UserFrame runs its list to the end: a unit made during the stage runs in it too.
    while (true) {
        ids.clear();
        entity_registry_.for_each_unit([&](Entity& e) {
            if (e.entity_id() > done) {
                ids.push_back(e.entity_id());
            }
        });
        if (ids.empty()) {
            break;
        }
        done = ids.back();
        for (const u32 id : ids) {
            Entity* e = entity_registry_.find(id);
            if (e && !e->destroyed() && e->is_unit()) {
                auto* unit = static_cast<Unit*>(e);
                unit->tick_commands(SECONDS_PER_TICK, ctx);
                if (!unit->destroyed()) {
                    unit->bank_reclaim(get_army(unit->army()));
                }
            }
        }
    }
}

void SimState::update_entities() {
    PROFILE_ZONE("Sim::entities");
    SimContext ctx = sim_context();

    stepping_events_.clear();
    economy_events_.for_each([&](EconomyEvent& evt) {
        if (evt.active() && evt.unit_id() != 0) {
            stepping_events_.emplace_back(evt.unit_id(), &evt);
        }
    });
    std::stable_sort(stepping_events_.begin(), stepping_events_.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });

    // Units on paths look ahead for others they would meet (M203c).
    if (!moho_pathing_) {
        steer_ground_units(*this);
    }

    followed_.clear();
    std::vector<u32>& walk = entity_registry_.walk();
    std::vector<u32> walked;
    walked.reserve(walk.size());
    std::unordered_map<u32, size_t> put_back;
    size_t last_step = 0;
    for (size_t i = 0; i < walk.size(); ++i) {
        const u32 id = walk[i];
        Entity* e = entity_registry_.find(id);
        if (!e || e->walk() != Entity::Walk::Stepping) {
            continue;
        }
        e->tick_scroller();
        Entity* parent = attach_parent(*e);
        // Moho's Entity::TaskTick returns -4 and CTaskStage::UserFrame runs it again at the end of
        // the frame; a wait no step has ended since the last (a cycle) is let go.
        if (parent && parent->walk() == Entity::Walk::Stepping &&
            parent->stepped_tick() != tick_count_ && parent->stepped_tick() == e->stepped_tick()) {
            const auto again = put_back.find(id);
            if (again == put_back.end() || again->second < last_step) {
                put_back[id] = i;
                walk.push_back(id);
                continue;
            }
        }
        last_step = i;
        if (parent) {
            followed_.push_back({id, parent->entity_id(), parent->pending_position(),
                                 parent->pending_orientation()});
            if (!e->is_collision_beam()) {
                follow_parent(*e, *parent);
            }
        }
        e->set_stepped_tick(tick_count_);
        const Entity::Walk next = step_entity(*e, parent != nullptr, ctx);
        e = entity_registry_.find(id);
        if (!e) {
            continue;
        }
        if (next == Entity::Walk::Stepping) {
            walked.push_back(id);
        } else {
            e->set_walk(next);
        }
    }
    walk = std::move(walked);
    stepping_events_.clear();
    separate_ground_units();
}

void SimState::separate_ground_units() {
    PROFILE_ZONE("Sim::separation");
    struct Body {
        Unit* unit;
        f32 x, z, r;
        bool moving;
        bool fixed; ///< held (SetImmobile): others make all the way
        bool sub;   ///< submerged: it meets only other submerged units
    };
    std::vector<Body> bodies;
    f32 widest = 0;
    entity_registry_.for_each_unit([&](Entity& e) {
        auto& u = static_cast<Unit&>(e);
        u.set_jostled(false);
        if (u.destroyed() || u.is_dying() || u.is_air_unit() || u.max_speed() <= 0 ||
            u.is_being_built() || u.transport_id() != 0 || u.parent_entity_id() != 0)
            return;
        const f32 r = u.separation_radius();
        if (r <= 0) return;
        const Vector3& p = u.position();
        bodies.push_back({&u, p.x, p.z, r, std::abs(u.ground_speed()) > 0.01f, u.immobile(),
                          u.layer() == "Sub"});
        widest = std::max(widest, r);
    });
    if (bodies.size() < 2) return;

    // Buckets of kCell units; each holds its bodies in id order, and cells are
    // walked in a fixed order, so every sum below runs in the same order.
    constexpr f32 kCell = 8.0f;
    const auto cell_of = [](f32 v) { return static_cast<i32>(std::floor(v / kCell)); };
    const auto key = [](i32 cx, i32 cz) {
        return (static_cast<i64>(cx) << 32) ^ static_cast<i64>(static_cast<u32>(cz));
    };
    std::unordered_map<i64, std::vector<u32>> buckets; // lookup only
    for (u32 i = 0; i < bodies.size(); ++i)
        buckets[key(cell_of(bodies[i].x), cell_of(bodies[i].z))].push_back(i);

    // Each overlapping pair is pushed apart by half the overlap a tick, which
    // settles a crowd without jitter.
    constexpr f32 kRelax = 0.5f;
    std::vector<f32> push_x(bodies.size(), 0.0f), push_z(bodies.size(), 0.0f);
    for (u32 i = 0; i < bodies.size(); ++i) {
        const Body& a = bodies[i];
        const i32 reach = static_cast<i32>(std::ceil((a.r + widest) / kCell));
        const i32 cx = cell_of(a.x), cz = cell_of(a.z);
        for (i32 dz = -reach; dz <= reach; ++dz) {
            for (i32 dx = -reach; dx <= reach; ++dx) {
                const auto it = buckets.find(key(cx + dx, cz + dz));
                if (it == buckets.end()) continue;
                for (const u32 j : it->second) {
                    if (j <= i) continue; // each pair once
                    const Body& b = bodies[j];
                    if (a.sub != b.sub || (a.fixed && b.fixed)) continue;
                    const f32 ox = a.x - b.x, oz = a.z - b.z;
                    const f32 reach_ab = a.r + b.r;
                    const f32 d2 = ox * ox + oz * oz;
                    if (d2 >= reach_ab * reach_ab) continue;
                    const f32 d = std::sqrt(d2);
                    // Exactly on top of each other: apart along x, the
                    // lower id to the east.
                    const f32 nx = d > 1e-4f ? ox / d : 1.0f;
                    const f32 nz = d > 1e-4f ? oz / d : 0.0f;
                    const f32 overlap = (reach_ab - d) * kRelax;
                    f32 share_a;
                    if (a.fixed != b.fixed) share_a = a.fixed ? 0.0f : 1.0f;
                    else if (a.moving != b.moving) share_a = a.moving ? 0.0f : 1.0f;
                    else share_a = (b.r * b.r) / (a.r * a.r + b.r * b.r);
                    push_x[i] += nx * overlap * share_a;
                    push_z[i] += nz * overlap * share_a;
                    push_x[j] -= nx * overlap * (1.0f - share_a);
                    push_z[j] -= nz * overlap * (1.0f - share_a);
                }
            }
        }
    }

    for (u32 i = 0; i < bodies.size(); ++i) {
        if (push_x[i] == 0 && push_z[i] == 0) continue;
        Unit& u = *bodies[i].unit;
        Vector3 p = u.pending_position();
        p.x += push_x[i];
        p.z += push_z[i];
        if (pathfinding_grid_) {
            u32 gx = 0, gz = 0;
            pathfinding_grid_->world_to_grid(p.x, p.z, gx, gz);
            if (!pathfinding_grid_->is_passable_for(gx, gz, u.layer(), u.naval_draft(),
                                                    u.is_amphibious() || u.is_hover()))
                continue;
        }
        // With Moho pathing, as its drive does (Moho's CUnitMotion): no push
        // onto ground the unit won't fit, from ground it does.
        if (moho_pathing_ && footprint_fits_at(u.footprint(), p.x, p.z) == 0 &&
            footprint_fits_at(u.footprint(), u.pending_position().x, u.pending_position().z) != 0) {
            continue;
        }
        // On the surface as it drives; a submarine keeps its depth.
        if (terrain_ && !bodies[i].sub) {
            const Unit::GroundStance stance =
                u.ground_stance(terrain_.get(), p.x, p.z, u.pending_orientation());
            p.y = stance.y;
            u.set_orientation(stance.orientation);
        }
        u.set_position(clamp_to_playable(p, u.army()));
        u.set_jostled(true);
    }
}

InfluenceMap* SimState::influence_map(i32 army) {
    ArmyBrain* brain = get_army(army);
    if (!brain || !terrain_) return nullptr;
    if (!brain->influence_map())
        brain->set_influence_map(std::make_unique<InfluenceMap>(
            terrain_->map_width(), terrain_->map_height(), static_cast<u32>(armies_.size())));
    return brain->influence_map();
}

CellRect SimState::playable_cells(const InfluenceMap& map) const {
    if (has_playable_rect_)
        return map.cells_in(playable_x0_, playable_z0_, playable_x1_, playable_z1_);
    return {0, 0, map.width() - 1, map.height() - 1};
}

namespace {
/// Moho's IsMobile: a motion type other than RULEUMT_None.
bool moves(const Unit& u) {
    return !u.motion_type().empty() && u.motion_type() != "RULEUMT_None";
}

/// The influence map's layer for a unit's threat (Moho's LAYER_ mask).
ThreatLayer threat_layer_of(const Unit& u) {
    const std::string& layer = u.layer();
    if (layer == "Land") return ThreatLayer::Land;
    if (layer == "Water" || layer == "Seabed" || layer == "Sub") return ThreatLayer::Naval;
    return ThreatLayer::None;
}

/// A unit that lives: not gone, dying or left as a wreck.
bool alive(const Entity* e) {
    return e && !e->destroyed() && e->is_unit() && !static_cast<const Unit*>(e)->is_dying() &&
           !e->is_wreckage();
}

/// What an influence entry keeps of `u` (its blueprint's threat levels).
ThreatSource threat_source_of(const Unit& u) {
    ThreatSource s;
    s.air = u.air_threat();
    s.surface = u.surface_threat();
    s.sub = u.sub_threat();
    s.economy = u.economy_threat();
    s.mobile = moves(u);
    s.flies = u.motion_type() == "RULEUMT_Air";
    static const CategoryName kMassExtraction{"MASSEXTRACTION"};
    static const CategoryName kExperimental{"EXPERIMENTAL"};
    static const CategoryName kCommand{"COMMAND"};
    s.mass_extractor = u.has_category(kMassExtraction);
    s.experimental = u.has_category(kExperimental);
    s.commander = u.has_category(kCommand);
    return s;
}
} // namespace

void SimState::feed_influence_map() {
    if (!intel_grids_) return;
    const u32 a = recon_army();
    if (a >= intel_grids_->armies()) {
        return;
    }
    InfluenceMap* map = influence_map(static_cast<i32>(a));
    if (!map) return;
    const i32 owner = static_cast<i32>(a);

    // Every unit of another army its intel detects -- an ally's always -- and
    // every structure it has once had in sight (a remembered blip).
    static const CategoryName kVisibleToRecon{"VISIBLETORECON"};
    entity_registry_.for_each_unit([&](Entity& e) {
        if (!alive(&e) || e.army() == owner) return;
        auto& u = static_cast<Unit&>(e);
        if (!u.has_category(kVisibleToRecon)) return;
        const bool detected = is_ally(owner, u.army()) || sensed_now(e.entity_id(), a);
        if (detected || (!moves(u) && ever_in_sight(e.entity_id(), a)))
            map->report(e.entity_id(), u.army(), u.position(), threat_source_of(u));
    });
    // And each jammer's fake it holds, sensed or not, as Moho's recon feeds
    // every blip (UpdateBlip): its jammer's threat at the fake's place.
    // Known fakes are fed too; the update zeroes them (M215f).
    for (const HeldFake& f : held_fakes(owner))
        map->report(fake_blip_id(f.jammer, f.index), f.unit->army(), f.position,
                    threat_source_of(*f.unit));

    // A dead structure's entry goes once the army sees where it stood, or
    // it was an ally's; a mobile unit's fades out instead.
    std::vector<u32> gone;
    map->for_each_entry([&](const InfluenceMap::EntryView& v) {
        if (v.mobile || (v.id & kFakeBlipBit) != 0 || alive(entity_registry_.find(v.id))) return;
        if (is_ally(owner, v.source_army) || (recon_at(v.position, a) & kReconLOS) != 0)
            gone.push_back(v.id);
    });
    for (const u32 id : gone) map->remove(id);
}

void SimState::update_influence_maps() {
    for (size_t i = 0; i < armies_.size(); ++i) {
        if (tick_count_ % 30 != i) continue;
        const i32 owner = static_cast<i32>(i);
        InfluenceMap* map = influence_map(owner);
        if (!map) continue;
        // The fakes the army holds now: their jammer's layer, never detailed
        // (one in sight or omni is a known fake), and whether it knows them
        // fake. One it holds no longer only fades, as a gone blip's entry.
        std::map<u32, InfluenceMap::UnitState> fakes;
        for (const HeldFake& f : held_fakes(owner))
            fakes[fake_blip_id(f.jammer, f.index)] = {threat_layer_of(*f.unit), false,
                                                      f.known_fake};
        map->update([&](i32 army) { return army == owner || is_ally(owner, army); },
                    [&](u32 id) -> std::optional<InfluenceMap::UnitState> {
                        if ((id & kFakeBlipBit) != 0) {
                            const auto fake = fakes.find(id);
                            if (fake == fakes.end()) return std::nullopt;
                            return fake->second;
                        }
                        const Entity* e = entity_registry_.find(id);
                        if (!alive(e)) return std::nullopt;
                        const auto& u = static_cast<const Unit&>(*e);
                        InfluenceMap::UnitState s;
                        s.layer = threat_layer_of(u);
                        const auto* recon = entity_recon(id);
                        s.detailed = ever_in_sight(id, static_cast<u32>(owner)) ||
                                     (recon && i < MAX_VIS_ARMIES && (*recon)[i].omni);
                        return s;
                    });
    }
}

u32 SimState::intel_sharers(u32 viewer) const {
    u32 mask = viewer < 32 ? 1u << viewer : 0;
    const u32 n = intel_grids_ ? intel_grids_->armies() : 0;
    for (u32 x = 0; x < n; ++x)
        if (x != viewer && is_ally(static_cast<i32>(x), static_cast<i32>(viewer))) mask |= 1u << x;
    return mask;
}

f32 SimState::water_surface() const {
    return terrain_ && terrain_->has_water() ? terrain_->water_elevation() : -10000.0f;
}

void SimState::paint_intel(i32 army, IntelSource source, const Vector3& pos, u32 radius, i8 delta) {
    if (!intel_grids_ || army < 0 || radius == 0) return;
    const map::IntelLayer layer = intel_source_layer(source);
    const u32 n = intel_grids_->armies();
    const auto paint = [&](u32 a) {
        map::IntelGrid& grid = intel_grids_->grid(a, layer);
        if (delta > 0) grid.add_circle(pos.x, pos.z, radius);
        else grid.sub_circle(pos.x, pos.z, radius);
    };
    if (intel_source_is_field(source)) {
        for (u32 a = 0; a < n; ++a)
            if (a != static_cast<u32>(army)) paint(a);
    } else if (static_cast<u32>(army) < n) {
        paint(static_cast<u32>(army));
    }
}

namespace {
/// A radius as Moho's handles keep it: whole units, never below 0.
u32 intel_radius(f32 r) {
    return r > 0.0f ? static_cast<u32>(r) : 0;
}
} // namespace

void SimState::sync_intel() {
    if (!terrain_) return;
    const auto armies =
        static_cast<u32>(std::min(army_count(), static_cast<size_t>(MAX_VIS_ARMIES)));
    if (!intel_grids_ || intel_grids_->armies() != armies ||
        intel_grids_->fog_of_war() != (fog_mode_ != FogMode::None))
        build_intel_grids();
    const u32 pass = ++intel_pass_;
    const u32 tick = tick_count_;

    // Visible areas (scrying): seen for their ticks, rubbed out the pass
    // after.
    std::erase_if(temp_visions_, [&](const TempVision& tv) {
        if (tv.remaining_ticks > 0) return false;
        if (tv.painted)
            paint_intel(static_cast<i32>(tv.army), IntelSource::Vision, {tv.x, 0, tv.z},
                        intel_radius(tv.radius), -1);
        return true;
    });
    for (TempVision& tv : temp_visions_) {
        if (!tv.painted)
            paint_intel(static_cast<i32>(tv.army), IntelSource::Vision, {tv.x, 0, tv.z},
                        intel_radius(tv.radius), 1);
        tv.painted = true;
        --tv.remaining_ticks;
    }

    // An entity's handles, as Moho's CIntel keeps them up to date: `wanted`
    // is each source's switch and radius.
    using Wanted = std::array<std::pair<bool, u32>, kIntelSources>;
    const auto track = [&](u32 id, i32 army, const Vector3& pos, const Wanted& wanted) {
        auto [it, fresh] = painted_intel_.try_emplace(id);
        PaintedIntel& rec = it->second;
        if (!fresh && rec.army != army) { // its intel given to another army
            for (size_t s = 0; s < kIntelSources; ++s)
                if (const IntelHandle& h = rec.handles[s]; h.enabled)
                    paint_intel(rec.army, static_cast<IntelSource>(s), h.pos, h.radius, -1);
            rec = PaintedIntel{};
            fresh = true;
        }
        if (fresh) {
            rec.army = army;
            rec.last_pos = pos;
            for (IntelHandle& h : rec.handles) h.pos = pos;
        }
        rec.pass = pass;
        const bool moved = !fresh && (pos.x != rec.last_pos.x || pos.y != rec.last_pos.y ||
                                      pos.z != rec.last_pos.z);
        const bool stopped = rec.moved && !moved;
        for (size_t s = 0; s < kIntelSources; ++s) {
            const auto source = static_cast<IntelSource>(s);
            IntelHandle& h = rec.handles[s];
            const auto [on, radius] = wanted[s];
            if (!h.enabled) {
                // Off, it follows its entity unpainted (UpdatePos); on, it
                // is painted where it is (EnableIntel's AddViz).
                h.pos = pos;
                h.radius = radius;
                if (on) {
                    h.enabled = true;
                    paint_intel(army, source, h.pos, h.radius, 1);
                }
                continue;
            }
            if (!on) { // DisableIntel's SubViz
                paint_intel(army, source, h.pos, h.radius, -1);
                h.enabled = false;
                h.pos = pos;
                h.radius = radius;
                continue;
            }
            if (radius != h.radius) { // ChangeRadius
                paint_intel(army, source, h.pos, h.radius, -1);
                h.radius = radius;
                paint_intel(army, source, h.pos, h.radius, 1);
            }
            // Moving, a third of its radius or 30 ticks on (UpdatePos);
            // stopping, at once (the motion's Stopped event).
            bool update = false;
            if (moved) {
                const f32 dx = pos.x - h.pos.x;
                const f32 dy = pos.y - h.pos.y;
                const f32 dz = pos.z - h.pos.z;
                const auto range = static_cast<f32>(static_cast<f64>(h.radius) * 0.333);
                update = dx * dx + dy * dy + dz * dz >= range * range ||
                         static_cast<i64>(tick) - static_cast<i64>(h.last_tick) > 30;
            }
            if (update || stopped) {
                h.last_tick = tick;
                if (pos.x != h.pos.x || pos.y != h.pos.y || pos.z != h.pos.z) {
                    paint_intel(army, source, h.pos, h.radius, -1);
                    h.pos = pos;
                    paint_intel(army, source, h.pos, h.radius, 1);
                }
            }
        }
        rec.moved = moved;
        rec.last_pos = pos;
    };

    // Each one's intel, in a walk of it (a few entries; no lookups by name)
    const auto wanted_of = [](const auto& intel) {
        Wanted wanted{};
        for (const auto& [name, state] : intel)
            if (const int s = intel_source_index(name); s >= 0)
                wanted[static_cast<size_t>(s)] = {state.enabled, intel_radius(state.radius)};
        return wanted;
    };
    entity_registry_.for_each_unit([&](Entity& e) {
        if (!e.is_unit()) {
            return;
        }
        const auto& unit = static_cast<const Unit&>(e);
        track(e.entity_id(), e.army(), e.position(), wanted_of(unit.intel_states()));
    });
    // Script entities' intel (VizMarkers), forgotten once the entity is gone.
    for (auto it = entity_intel_.begin(); it != entity_intel_.end();) {
        const Entity* e = entity_registry_.find(it->first);
        if (!e) {
            it = entity_intel_.erase(it);
            continue;
        }
        track(it->first, it->second.army, e->position(), wanted_of(it->second.sources));
        ++it;
    }
    // What is gone takes its intel with it.
    for (auto it = painted_intel_.begin(); it != painted_intel_.end();) {
        if (it->second.pass == pass) {
            ++it;
            continue;
        }
        for (size_t s = 0; s < kIntelSources; ++s)
            if (const IntelHandle& h = it->second.handles[s]; h.enabled)
                paint_intel(it->second.army, static_cast<IntelSource>(s), h.pos, h.radius, -1);
        it = painted_intel_.erase(it);
    }
}

u8 SimState::recon_senses(const Vector3& pos, u32 sharers, bool below, bool sonar) const {
    const bool fog = intel_grids_->fog_of_war();
    u8 f = fog ? 0 : kReconLOS; // no fog: line of sight everywhere
    const map::IntelLayer sight = below ? map::IntelLayer::Water : map::IntelLayer::Vision;
    for (u32 a = 0; a < intel_grids_->armies(); ++a) {
        if ((sharers >> a & 1u) == 0) continue;
        if (fog && intel_grids_->grid(a, sight).visible(pos.x, pos.z)) f |= kReconLOS;
        if (sonar && intel_grids_->grid(a, map::IntelLayer::Sonar).visible(pos.x, pos.z))
            f |= kReconSonar;
        if (!below && intel_grids_->grid(a, map::IntelLayer::Radar).visible(pos.x, pos.z))
            f |= kReconRadar;
        if (intel_grids_->grid(a, map::IntelLayer::Omni).visible(pos.x, pos.z)) f |= kReconOmni;
    }
    return f;
}

u8 SimState::recon_counters(const Vector3& pos, u32 viewer, u8 senses, const Unit* unit) const {
    u8 f = senses;
    if (f == 0 || (f & kReconOmni) != 0) return f; // omni beats every counter
    const auto here = [&](map::IntelLayer layer) {
        return intel_grids_->grid(viewer, layer).visible(pos.x, pos.z);
    };
    if (here(map::IntelLayer::RadarCounter)) f &= static_cast<u8>(~kReconRadar);
    if (here(map::IntelLayer::SonarCounter)) f &= static_cast<u8>(~kReconSonar);
    if ((unit && unit->is_cloaked()) || here(map::IntelLayer::VisionCounter))
        f &= static_cast<u8>(~kReconLOS);
    // A unit's own stealth only out of sight
    if (unit && (f & kReconLOS) == 0) {
        if (unit->has_radar_stealth()) f &= static_cast<u8>(~kReconRadar);
        if (unit->has_sonar_stealth()) f &= static_cast<u8>(~kReconSonar);
    }
    return f;
}

u8 SimState::recon_detect(const Entity& e, const Vector3& pos, u32 viewer, u32 sharers) const {
    if (!intel_grids_ || viewer >= intel_grids_->armies()) return 0;
    const Vector3 kept = clamp_to_playable(pos, static_cast<i32>(viewer));
    if (kept.x != pos.x || kept.z != pos.z) return 0; // off the playable area
    if (e.army() == static_cast<i32>(viewer) || is_ally(static_cast<i32>(viewer), e.army()))
        return kReconAll;
    if (!e.is_unit()) {
        const bool below = water_surface() > pos.y;
        return recon_counters(pos, viewer, recon_senses(pos, sharers, below, below), nullptr);
    }
    const auto& unit = static_cast<const Unit&>(e);
    const std::string& layer = unit.layer();
    const bool below = layer == "Seabed" || layer == "Sub";
    const bool sonar = below || layer == "Water";
    return recon_counters(pos, viewer, recon_senses(pos, sharers, below, sonar), &unit);
}

u8 SimState::recon_of(const Entity& e, const Vector3& pos, u32 viewer) const {
    return recon_detect(e, pos, viewer, intel_sharers(viewer));
}

u8 SimState::recon_at(const Vector3& pos, u32 viewer) const {
    if (!intel_grids_ || viewer >= intel_grids_->armies()) return 0;
    const bool below = water_surface() > pos.y;
    return recon_counters(pos, viewer, recon_senses(pos, intel_sharers(viewer), below, below),
                          nullptr);
}

u8 SimState::recon_in(f32 x0, f32 z0, f32 x1, f32 z1, f32 y, u32 viewer) const {
    if (!intel_grids_ || viewer >= intel_grids_->armies()) return 0;
    const bool below = water_surface() > y;
    const bool fog = intel_grids_->fog_of_war();
    const u32 sharers = intel_sharers(viewer);
    const auto any = [&](u32 a, map::IntelLayer layer) {
        return intel_grids_->grid(a, layer).any_in(x0, z0, x1, z1);
    };
    u8 f = fog ? 0 : kReconLOS;
    for (u32 a = 0; a < intel_grids_->armies(); ++a) {
        if ((sharers >> a & 1u) == 0) continue;
        if (fog && any(a, below ? map::IntelLayer::Water : map::IntelLayer::Vision)) f |= kReconLOS;
        if (any(a, below ? map::IntelLayer::Sonar : map::IntelLayer::Radar))
            f |= below ? kReconSonar : kReconRadar;
        if (any(a, map::IntelLayer::Omni)) f |= kReconOmni;
    }
    if (f == 0 || (f & kReconOmni) != 0) return f;
    if (any(viewer, map::IntelLayer::RadarCounter)) f &= static_cast<u8>(~kReconRadar);
    if (any(viewer, map::IntelLayer::SonarCounter)) f &= static_cast<u8>(~kReconSonar);
    if (any(viewer, map::IntelLayer::VisionCounter)) f &= static_cast<u8>(~kReconLOS);
    return f;
}

void SimState::update_visibility() {
    PROFILE_ZONE("Sim::visibility");
    sync_intel();
    if (!intel_grids_) {
        return;
    }
    refresh_blips();
    const u32 army = recon_army();
    if (army < intel_grids_->armies()) {
        recon_tick(army);
    }
}

bool SimState::holds_blip(const Entity& e, u32 army) const {
    if (recon_flags(e.entity_id(), army).any()) {
        return true;
    }
    return e.is_unit() && !static_cast<const Unit&>(e).is_mobile() &&
           ever_in_sight(e.entity_id(), army);
}

// Sim::RefreshBlips: every blip follows its source, whatever its army senses
// (FR sim/ReconBlip.cpp:977).
void SimState::refresh_blips() {
    for (auto& [id, snaps] : blip_cache_) {
        const Entity* e = entity_registry_.find(id);
        if (!e || e->destroyed()) {
            continue;
        }
        const auto vis = prev_entity_vis_.find(id);
        const auto ever = los_ever_.find(id);
        const bool kept_seen =
            e->is_unit() && !static_cast<const Unit&>(*e).is_mobile() && ever != los_ever_.end();
        for (u32 a = 0; a < MAX_VIS_ARMIES; ++a) {
            BlipSnapshot& snap = snaps[a];
            if (snap.entity_army < 0) {
                continue;
            }
            const bool held = (vis != prev_entity_vis_.end() && vis->second[a].any()) ||
                              (kept_seen && ((ever->second >> a) & 1u) != 0);
            if (!held) {
                continue;
            }
            snap.last_known_position = e->pending_position();
            if (snap.blueprint_id != e->blueprint_id()) {
                snap.blueprint_id = e->blueprint_id();
            }
            snap.entity_army = e->army();
        }
    }
}

void SimState::forget_gone(u32 army) {
    const auto gone = [&](u32 id) {
        const Entity* e = entity_registry_.find(id);
        return !e || e->destroyed();
    };
    for (auto it = prev_entity_vis_.begin(); it != prev_entity_vis_.end();) {
        if (!gone(it->first)) {
            ++it;
            continue;
        }
        it->second[army] = {};
        const bool held = std::any_of(it->second.begin(), it->second.end(),
                                      [](const EntityVisSnapshot& f) { return f.any(); });
        it = held ? std::next(it) : prev_entity_vis_.erase(it);
    }
    for (auto it = blip_cache_.begin(); it != blip_cache_.end();) {
        if (!gone(it->first)) {
            ++it;
            continue;
        }
        it->second[army] = BlipSnapshot{};
        const bool held = std::any_of(it->second.begin(), it->second.end(),
                                      [](const BlipSnapshot& b) { return b.entity_army >= 0; });
        it = held ? std::next(it) : blip_cache_.erase(it);
    }
    for (auto it = los_ever_.begin(); it != los_ever_.end();) {
        if (gone(it->first) && army < 32) {
            it->second &= ~(1u << army);
        }
        it = it->second == 0 ? los_ever_.erase(it) : std::next(it);
    }
    destroy_gone_blips(army);
}

// CAiReconDBImpl::ReconTick (FR ai/CAiReconDBImpl.cpp:520): a held blip's changes in unit order,
// then the new blips', each OnIntelChange before its OnDetectedBy (GenerateNewBlips).
void SimState::recon_tick(u32 army) {
    forget_gone(army);
    const u32 sharers = intel_sharers(army);
    const u32 bit = 1u << army;
    const std::vector<u32> ids = units_by_army();
    const auto fire = [&](u32 id, const EntityVisSnapshot& was, const EntityVisSnapshot& now) {
        if (was.vision != now.vision) {
            fire_on_intel_change(id, army, "LOSNow", now.vision);
        }
        if (was.radar != now.radar) {
            fire_on_intel_change(id, army, "Radar", now.radar);
        }
        if (was.sonar != now.sonar) {
            fire_on_intel_change(id, army, "Sonar", now.sonar);
        }
        if (was.omni != now.omni) {
            fire_on_intel_change(id, army, "Omni", now.omni);
        }
    };
    struct Fresh {
        u32 id;
        EntityVisSnapshot now;
    };
    std::vector<Fresh> fresh;
    std::set<u32> held;
    for (const u32 id : ids) {
        const Entity* e = entity_registry_.find(id);
        if (!e || e->destroyed()) {
            continue;
        }
        const u8 f = recon_detect(*e, e->position(), army, sharers);
        const EntityVisSnapshot now{(f & kReconLOS) != 0, (f & kReconRadar) != 0,
                                    (f & kReconSonar) != 0, (f & kReconOmni) != 0};
        const bool own = e->army() == static_cast<i32>(army);
        const bool had = !own && holds_blip(*e, army);
        if (!own && !had && now.any()) {
            fresh.push_back({id, now});
            continue;
        }
        EntityVisSnapshot& slot = prev_entity_vis_[id][army];
        const EntityVisSnapshot was = slot;
        slot = now;
        if (now.vision) {
            los_ever_[id] |= bit;
        }
        if (had) {
            held.insert(id);
            fire(id, was, now);
        }
    }
    for (const Fresh& f : fresh) {
        const Entity* e = entity_registry_.find(f.id);
        if (!e || e->destroyed()) {
            continue;
        }
        prev_entity_vis_[f.id][army] = f.now;
        if (f.now.vision) {
            los_ever_[f.id] |= bit;
        }
        BlipSnapshot& snap = blip_cache_[f.id][army];
        const bool first = snap.entity_army < 0;
        snap.last_known_position = e->pending_position();
        snap.blueprint_id = e->blueprint_id();
        snap.entity_army = e->army();
        snap.entity_dead = false;
        fire(f.id, EntityVisSnapshot{}, f.now);
        if (first) {
            fire_on_detected_by(f.id, army);
        }
    }
    update_jam_blips(army, held);
}

Vector3 SimState::jam_offset(const Unit& u) {
    // Moho's ComputeJamOffset: Min plus (Max - Min) times a 32-bit draw, in
    // whole units, then a random fraction of that along a random heading.
    // (Moho normalizes two Gaussian draws; a point drawn in the unit disc is
    // as even in heading, and exact on every platform.)
    const auto lo = static_cast<u32>(std::max(0.0f, u.jam_radius_min()));
    const auto hi = static_cast<u32>(std::max(0.0f, u.jam_radius_max()));
    const u32 range = hi > lo ? hi - lo : 0;
    const u32 step = static_cast<u32>((static_cast<u64>(range) * sim_random_.next_u32()) >> 32);
    const auto radius = static_cast<f32>(lo + step);
    f32 x = 0;
    f32 z = 0;
    f32 len2 = 0;
    do {
        x = sim_random_.range(-1.0f, 1.0f);
        z = sim_random_.range(-1.0f, 1.0f);
        len2 = x * x + z * z;
    } while (len2 > 1.0f || len2 < 1e-6f);
    const f32 len = std::sqrt(len2);
    const f32 reach = sim_random_.range(0.0f, 1.0f) * radius;
    return {x / len * reach, 0.0f, z / len * reach};
}

// UpdateBlips keeps a jammer's fakes only where its army held the jammer's blip before this tick.
std::vector<u32> SimState::units_by_army() const {
    std::vector<std::pair<i32, u32>> order;
    order.reserve(entity_registry_.count());
    entity_registry_.for_each_unit([&](Entity& e) {
        if (!e.destroyed()) {
            order.emplace_back(e.army(), e.entity_id());
        }
    });
    std::sort(order.begin(), order.end());
    std::vector<u32> ids;
    ids.reserve(order.size());
    for (const auto& [owner, id] : order) {
        ids.push_back(id);
    }
    return ids;
}

void SimState::update_jam_blips(u32 army, const std::set<u32>& held) {
    std::set<u64> kept;
    for (const u32 id : units_by_army()) {
        Entity* found = entity_registry_.find(id);
        if (!found || found->destroyed()) {
            continue;
        }
        Entity& e = *found;
        auto& u = static_cast<Unit&>(e);
        if (u.jammer_blips() == 0 || u.is_dying() || u.is_being_built() ||
            !u.is_intel_enabled("Jammer")) {
            continue;
        }
        const i32 owner = u.army();
        if (static_cast<i32>(army) == owner || is_ally(static_cast<i32>(army), owner)) {
            continue;
        }
        if (held.count(u.entity_id()) == 0 || !holds_blip(u, army)) {
            continue;
        }
        const u64 key = jam_key(u.entity_id(), army);
        auto& offsets = jam_offsets_[key];
        while (offsets.size() < u.jammer_blips()) {
            offsets.push_back(jam_offset(u));
        }
        offsets.resize(u.jammer_blips());
        kept.insert(key);
        u64 known = jam_known_[key];
        u64 sensed = 0;
        for (size_t i = 0; i < offsets.size() && i < 64; ++i) {
            const Vector3 at{u.position().x + offsets[i].x, u.position().y + offsets[i].y,
                             u.position().z + offsets[i].z};
            if (recon_of(u, at, army) != 0) {
                sensed |= u64{1} << i;
            }
            if (fake_known_now(u, at, army)) {
                known |= u64{1} << i;
            }
        }
        if (offsets.size() < 64) {
            known &= (u64{1} << offsets.size()) - 1;
        }
        if (known == 0) {
            jam_known_.erase(key);
        } else {
            jam_known_[key] = known;
        }
        if (sensed == 0) {
            jam_sensed_.erase(key);
        } else {
            jam_sensed_[key] = sensed;
        }
    }
    const auto dropped = [&](const auto& entry) {
        return (entry.first & 0xFF) == army && kept.count(entry.first) == 0;
    };
    std::erase_if(jam_offsets_, dropped);
    std::erase_if(jam_known_, dropped);
    std::erase_if(jam_sensed_, dropped);
}

bool SimState::fake_known_now(const Unit& jammer, const Vector3& pos, u32 army) const {
    if ((recon_of(jammer, pos, army) & (kReconLOS | kReconOmni)) != 0) return true;
    // Moho's IsWithinPlayableMapRadius: the map, and the playable area unless
    // the army may use the whole map, each side inset by the jammer's larger
    // footprint side.
    f32 x0 = 0, z0 = 0;
    f32 x1 = terrain_ ? static_cast<f32>(terrain_->map_width()) : 1e9f;
    f32 z1 = terrain_ ? static_cast<f32>(terrain_->map_height()) : 1e9f;
    const ArmyBrain* brain = army_at(army);
    if (has_playable_rect_ && !(brain && brain->use_whole_map())) {
        x0 = std::max(x0, playable_x0_);
        z0 = std::max(z0, playable_z0_);
        x1 = std::min(x1, playable_x1_);
        z1 = std::min(z1, playable_z1_);
    }
    const blueprints::Footprint& fp = jammer.footprint(); // SFootprint's whole cells
    const auto inset = static_cast<f32>(std::max(fp.size_x, fp.size_z));
    return !(pos.x >= x0 + inset && pos.x <= x1 - inset && pos.z >= z0 + inset &&
             pos.z <= z1 - inset);
}

std::vector<SimState::HeldFake> SimState::held_fakes(i32 viewer) const {
    std::vector<HeldFake> held;
    if (!intel_grids_) return held;
    for (const auto& [key, offsets] : jam_offsets_) {
        const auto army = static_cast<u32>(key & 0xFF);
        if (viewer >= 0 && army != static_cast<u32>(viewer)) continue;
        const Entity* e = entity_registry_.find(static_cast<u32>(key >> 8));
        if (!e || e->destroyed() || !e->is_unit()) continue;
        const auto& u = static_cast<const Unit&>(*e);
        for (size_t i = 0; i < offsets.size(); ++i) {
            HeldFake f;
            f.jammer = u.entity_id();
            f.unit = &u;
            f.viewer = army;
            f.index = static_cast<u32>(i);
            f.position = {u.position().x + offsets[i].x, u.position().y + offsets[i].y,
                          u.position().z + offsets[i].z};
            const auto sensed = jam_sensed_.find(key);
            f.sensed = sensed != jam_sensed_.end() && i < 64 && ((sensed->second >> i) & 1) != 0;
            const auto latched = jam_known_.find(key);
            f.known_fake =
                latched != jam_known_.end() && i < 64 && ((latched->second >> i) & 1) != 0;
            held.push_back(f);
        }
    }
    return held;
}

bool SimState::unit_cap_allows(i32 army, f32 cap_cost, bool tell) {
    ArmyBrain* brain = get_army(army);
    if (!brain || brain->ignores_unit_cap() || brain->unit_cap() < 0) return true;
    if (brain->get_unit_cost_total(entity_registry_) + cap_cost <=
        static_cast<f32>(brain->unit_cap()))
        return true;
    if (tell) brain_hears(army, "OnUnitCapLimitReached");
    return false;
}

void SimState::brain_hears(i32 army, const char* method) {
    ArmyBrain* brain = get_army(army);
    if (!brain || !L_ || brain->lua_table_ref() < 0) return;
    lua_rawgeti(L_, LUA_REGISTRYINDEX, brain->lua_table_ref());
    lua_pushstring(L_, method);
    lua_gettable(L_, -2);
    if (lua_isfunction(L_, -1)) {
        lua_pushvalue(L_, -2);
        if (lua_pcall(L_, 1, 0, 0) != 0) {
            spdlog::warn("{} error: {}", method, lua_tostring(L_, -1));
            lua_pop(L_, 1);
        }
    } else {
        lua_pop(L_, 1);
    }
    lua_pop(L_, 1);
}

void SimState::fire_on_intel_change(u32 entity_id, u32 army_idx,
                                    const char* recon_type, bool val) {
    auto* brain = get_army(static_cast<i32>(army_idx));
    if (!brain || brain->lua_table_ref() < 0) return;

    auto* entity = entity_registry_.find(entity_id);
    if (!entity || entity->destroyed()) return;

    lua_rawgeti(L_, LUA_REGISTRYINDEX, brain->lua_table_ref());
    int brain_tbl = lua_gettop(L_);

    lua_pushstring(L_, "OnIntelChange");
    lua_gettable(L_, brain_tbl);
    if (!lua_isfunction(L_, -1)) {
        lua_pop(L_, 2); // pop non-function + brain_tbl
        return;
    }

    lua_pushvalue(L_, brain_tbl); // self (brain)

    // The army's blip of the unit (push_blip)
    push_blip(L_, entity->entity_id(), static_cast<i32>(army_idx));

    lua_pushstring(L_, recon_type);
    lua_pushboolean(L_, val ? 1 : 0);

    if (lua_pcall(L_, 4, 0, 0) != 0) {
        spdlog::warn("OnIntelChange error: {}", lua_tostring(L_, -1));
        lua_pop(L_, 1);
    }

    lua_pop(L_, 1); // pop brain_tbl
}

void SimState::flush_intel_in_rect(i32 x0, i32 z0, i32 x1, i32 z1) {
    const auto inside = [&](const Vector3& p) {
        return p.x >= static_cast<f32>(x0) && p.x <= static_cast<f32>(x1) &&
               p.z >= static_cast<f32>(z0) && p.z <= static_cast<f32>(z1);
    };
    IntelFlushEvent event{x0, z0, x1, z1, {}};
    std::vector<u32> ids;
    ids.reserve(blip_cache_.size());
    for (const auto& entry : blip_cache_) ids.push_back(entry.first);
    std::sort(ids.begin(), ids.end());
    std::map<u32, u32> forgotten; // id -> armies
    const u32 n = static_cast<u32>(std::min(army_count(), static_cast<size_t>(MAX_VIS_ARMIES)));
    // Army by army, as Moho flushes each army's recon in turn. Looked up
    // afresh each time: OnIntelChange runs scripts.
    for (u32 a = 0; a < n; ++a) {
        for (const u32 id : ids) {
            const auto it = blip_cache_.find(id);
            if (it == blip_cache_.end()) continue;
            BlipSnapshot& snap = it->second[a];
            if (snap.entity_army < 0 || !inside(snap.last_known_position)) continue;
            if (const auto* e = entity_registry_.find(id); e && e->is_unit()) {
                const std::string& layer = static_cast<const Unit*>(e)->layer();
                if (layer == "Sub" || layer == "None" || layer.empty()) continue;
            }
            snap = BlipSnapshot{};
            forgotten[id] |= 1u << a;
            if (const auto ever = los_ever_.find(id); ever != los_ever_.end()) {
                ever->second &= ~(1u << a);
                if (ever->second == 0) los_ever_.erase(ever);
            }
            EntityVisSnapshot was;
            if (const auto vis = prev_entity_vis_.find(id); vis != prev_entity_vis_.end()) {
                was = vis->second[a];
                vis->second[a] = EntityVisSnapshot{};
            }
            if (was.vision) fire_on_intel_change(id, a, "LOSNow", false);
            if (was.radar) fire_on_intel_change(id, a, "Radar", false);
            if (was.sonar) fire_on_intel_change(id, a, "Sonar", false);
            if (was.omni) fire_on_intel_change(id, a, "Omni", false);
        }
    }
    event.forgotten.assign(forgotten.begin(), forgotten.end());
    intel_flush_events_.push_back(std::move(event));
}

void SimState::fire_on_detected_by(u32 entity_id, u32 army_idx) {
    auto* entity = entity_registry_.find(entity_id);
    if (!L_ || !entity || entity->destroyed() || entity->lua_table_ref() < 0) return;
    lua_rawgeti(L_, LUA_REGISTRYINDEX, entity->lua_table_ref());
    const int unit_tbl = lua_gettop(L_);
    lua_pushstring(L_, "OnDetectedBy");
    lua_gettable(L_, unit_tbl);
    if (lua_isfunction(L_, -1)) {
        lua_pushvalue(L_, unit_tbl);
        lua_pushnumber(L_, static_cast<lua_Number>(army_idx + 1));
        if (lua_pcall(L_, 2, 0, 0) != 0) {
            spdlog::warn("OnDetectedBy error: {}", lua_tostring(L_, -1));
            lua_pop(L_, 1);
        }
    } else {
        lua_pop(L_, 1);
    }
    lua_pop(L_, 1); // unit_tbl
}

namespace {

constexpr const char* kBlipsKey = "__osc_blips";
constexpr const char* kBlipClassKey = "__osc_blip_class";

/// Push the class blips are made of: /lua/sim/Blip.lua's Blip, as Moho's
/// ReconBlip imports it, else a class of moho.blip_methods alone. Resolved
/// once, then kept in the registry.
void push_blip_class(lua_State* L) {
    lua_pushstring(L, kBlipClassKey);
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (lua_istable(L, -1)) return;
    lua_pop(L, 1);
    const int top = lua_gettop(L);
    bool found = false;
    lua_pushstring(L, "import");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (lua_isfunction(L, -1)) {
        lua_pushstring(L, "/lua/sim/Blip.lua");
        if (lua_pcall(L, 1, 1, 0) != 0) {
            spdlog::warn("/lua/sim/Blip.lua: {}", lua_tostring(L, -1));
        } else if (lua_istable(L, -1)) {
            lua_pushstring(L, "Blip");
            lua_rawget(L, -2);
            found = lua_istable(L, -1);
        }
    }
    if (found) {
        lua_replace(L, top + 1); // the class, over the module
        lua_settop(L, top + 1);
    } else {
        lua_settop(L, top);
        lua_newtable(L); // {__index = moho.blip_methods}
        lua_pushstring(L, "__index");
        lua_pushstring(L, "moho");
        lua_rawget(L, LUA_GLOBALSINDEX);
        if (lua_istable(L, -1)) {
            lua_pushstring(L, "blip_methods");
            lua_rawget(L, -2);
            lua_remove(L, -2);
        } else {
            lua_pop(L, 1);
            lua_pushnil(L);
        }
        lua_rawset(L, -3);
    }
    lua_pushstring(L, kBlipClassKey);
    lua_pushvalue(L, -2);
    lua_rawset(L, LUA_REGISTRYINDEX);
}

/// Push the registry's blip store (entity id -> army + 1 -> blip), made if
/// missing.
void push_blip_store(lua_State* L) {
    lua_pushstring(L, kBlipsKey);
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (lua_istable(L, -1)) return;
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushstring(L, kBlipsKey);
    lua_pushvalue(L, -2);
    lua_rawset(L, LUA_REGISTRYINDEX);
}

} // namespace

void SimState::push_blip(lua_State* L, u32 entity_id, i32 army) {
    const auto make = [&] {
        push_blip_class(L);
        push_new_script_object(L, "Blip");
        const int blip = lua_gettop(L);
        lua_pushstring(L, "_c_entity_id");
        lua_pushnumber(L, static_cast<lua_Number>(entity_id));
        lua_rawset(L, blip);
        lua_pushstring(L, "_c_req_army");
        lua_pushnumber(L, static_cast<lua_Number>(army));
        lua_rawset(L, blip);
    };
    if (army < 0) {
        make();
        return;
    }
    push_blip_store(L);
    const int store = lua_gettop(L);
    lua_rawgeti(L, store, static_cast<int>(entity_id));
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_rawseti(L, store, static_cast<int>(entity_id));
    }
    const int per_entity = lua_gettop(L);
    lua_rawgeti(L, per_entity, army + 1);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        make();
        lua_pushvalue(L, -1);
        lua_rawseti(L, per_entity, army + 1);
        blip_objects_.insert(entity_id);
    }
    lua_replace(L, store);
    lua_settop(L, store);
}

void SimState::destroy_gone_blips(u32 army) {
    if (!L_ || blip_objects_.empty()) {
        return;
    }
    std::vector<u32> gone;
    for (const u32 id : blip_objects_) {
        const Entity* e = entity_registry_.find(id);
        if (!e || e->destroyed()) {
            gone.push_back(id);
        }
    }
    if (gone.empty()) {
        return;
    }
    const u32 recon_armies = intel_grids_ ? intel_grids_->armies() : 0;
    push_blip_store(L_);
    const int store = lua_gettop(L_);
    for (const u32 id : gone) {
        lua_rawgeti(L_, store, static_cast<int>(id));
        const int per_entity = lua_gettop(L_);
        if (lua_istable(L_, per_entity)) {
            for (u32 a = 0; a < MAX_VIS_ARMIES; ++a) {
                if (a != army && a < recon_armies) {
                    continue;
                }
                lua_rawgeti(L_, per_entity, static_cast<int>(a + 1));
                const int blip = lua_gettop(L_);
                if (lua_istable(L_, blip)) {
                    // Forgotten first: a hook asking for it again doesn't get it back.
                    lua_pushnil(L_);
                    lua_rawseti(L_, per_entity, static_cast<int>(a + 1));
                    lua_pushstring(L_, "_c_gone");
                    lua_pushboolean(L_, 1);
                    lua_rawset(L_, blip);
                    lua_pushstring(L_, "OnDestroy");
                    lua_gettable(L_, blip);
                    if (lua_isfunction(L_, -1)) {
                        lua_pushvalue(L_, blip);
                        if (lua_pcall(L_, 1, 0, 0) != 0) {
                            spdlog::warn("Blip OnDestroy error: {}", lua_tostring(L_, -1));
                        }
                    }
                }
                lua_settop(L_, per_entity);
            }
            lua_pushnil(L_);
            if (lua_next(L_, per_entity) != 0) {
                lua_settop(L_, store);
                continue;
            }
        }
        blip_objects_.erase(id);
        lua_pushnil(L_);
        lua_rawseti(L_, store, static_cast<int>(id));
        lua_settop(L_, store);
    }
    lua_pop(L_, 1);
}

namespace {
constexpr u32 kVictoryGraceTicks = 50; // ~5s: let armies spawn before eliminating
} // namespace

i32 SimState::find_share_recipient(i32 defeated_army) const {
    switch (share_mode_) {
    // FullShare and PartialShare hand (some) units to a surviving ally. FA gives
    // to the highest-rated ally; we approximate with the first living ally.
    case ShareMode::FullShare:
    case ShareMode::PartialShare: {
        for (size_t i = 0; i < armies_.size(); ++i) {
            if (static_cast<i32>(i) == defeated_army) continue;
            const auto& b = armies_[i];
            if (b->is_civilian() || b->is_defeated()) continue;
            if (is_ally(defeated_army, static_cast<i32>(i)))
                return static_cast<i32>(i);
        }
        return -1;
    }
    case ShareMode::Defectors: {
        // Units defect to a surviving enemy (FA: highest-rated enemy).
        for (size_t i = 0; i < armies_.size(); ++i) {
            if (static_cast<i32>(i) == defeated_army) continue;
            const auto& b = armies_[i];
            if (b->is_civilian() || b->is_defeated()) continue;
            if (is_enemy(defeated_army, static_cast<i32>(i)))
                return static_cast<i32>(i);
        }
        return -1;
    }
    case ShareMode::CivilianDeserter: {
        for (size_t i = 0; i < armies_.size(); ++i) {
            if (armies_[i]->is_civilian()) return static_cast<i32>(i);
        }
        return -1;
    }
    default:
        // ShareUntilDeath destroys the units; TransferToKiller would need
        // per-unit killer attribution (not modeled) so it also destroys.
        return -1;
    }
}

void SimState::defeat_army(i32 army) {
    if (army < 0 || static_cast<size_t>(army) >= armies_.size()) return;
    // Once the match is decided a quiet peer is a player leaving the score
    // screen, not a drop: the result stands.
    if (game_ended_) return;
    auto& b = armies_[static_cast<size_t>(army)];
    if (!b || b->is_defeated()) return; // idempotent
    b->set_state(BrainState::Defeat);
    dispose_defeated_army(army);
    spdlog::info("Army {} ({}) defeated (player drop)", army, b->name());
}

void SimState::kill_unit(Unit& unit, const char* damage_type) {
    if (unit.destroyed() || unit.is_dying()) return;
    if (L_ && unit.lua_table_ref() >= 0) {
        const int top = lua_gettop(L_);
        lua_rawgeti(L_, LUA_REGISTRYINDEX, unit.lua_table_ref());
        lua_pushstring(L_, "Kill");
        lua_gettable(L_, -2);
        if (lua_isfunction(L_, -1)) {
            lua_pushvalue(L_, top + 1);
            int args = 1;
            if (damage_type) {
                lua_pushnil(L_); // no instigator
                lua_pushstring(L_, damage_type);
                lua_pushnumber(L_, 0); // no excess damage
                args = 4;
            }
            if (lua_pcall(L_, args, 0, 0) != 0) {
                const char* err = lua_tostring(L_, -1);
                spdlog::warn("Kill error: {}", err ? err : "(unknown)");
            }
            lua_settop(L_, top);
            return;
        }
        lua_settop(L_, top);
    }
    unit.begin_dying();
    entity_registry_.destroy(unit);
}

bool SimState::give_unit(Unit& unit, i32 army) {
    // Through the script global, as a script gives one (ChangeUnitArmy: a
    // new unit of the army takes its place, M206v). A sim without the
    // bindings (unit tests) moves the unit itself.
    if (L_ && unit.lua_table_ref() >= 0) {
        const int top = lua_gettop(L_);
        lua_pushstring(L_, "ChangeUnitArmy");
        lua_rawget(L_, LUA_GLOBALSINDEX);
        if (lua_isfunction(L_, -1)) {
            lua_rawgeti(L_, LUA_REGISTRYINDEX, unit.lua_table_ref());
            lua_pushnumber(L_, army + 1);
            bool given = false;
            if (lua_pcall(L_, 2, 1, 0) != 0) {
                const char* err = lua_tostring(L_, -1);
                spdlog::warn("ChangeUnitArmy error: {}", err ? err : "(unknown)");
            } else {
                given = !lua_isnil(L_, -1);
            }
            lua_settop(L_, top);
            return given;
        }
        lua_settop(L_, top);
    }
    unit.set_army(army);
    return true;
}

void SimState::dispose_defeated_army(i32 army) {
    const i32 recipient = find_share_recipient(army);
    // PartialShare transfers only structures + engineers; the rest are destroyed.
    const bool partial = share_mode_ == ShareMode::PartialShare;
    bool transferred_any = false;
    std::vector<u32> to_give, to_kill;
    entity_registry_.for_each_unit([&](Entity& e) {
        if (e.army() != army || e.destroyed() || !e.is_unit()) return;
        auto* u = static_cast<Unit*>(&e);
        bool transfer = recipient >= 0;
        if (transfer && partial) {
            transfer = u->has_category("STRUCTURE") || u->has_category("ENGINEER");
        }
        if (transfer) to_give.push_back(u->entity_id());
        else if (!u->is_dying()) to_kill.push_back(u->entity_id());
    });
    // Given and killed after the walk: a transfer makes units, a death runs
    // scripts, which may kill others.
    for (const u32 id : to_give) {
        Entity* e = entity_registry_.find(id);
        if (e && !e->destroyed() && e->is_unit() && e->army() == army) {
            if (give_unit(static_cast<Unit&>(*e), recipient)) {
                transferred_any = true;
            } else {
                to_kill.push_back(id);
            }
        }
    }
    for (const u32 id : to_kill) {
        Entity* e = entity_registry_.find(id);
        if (e && !e->destroyed() && e->is_unit()) kill_unit(static_cast<Unit&>(*e));
    }
    if (transferred_any) {
        armies_[recipient]->note_has_units();
        spdlog::info("Army {} units transferred to army {} on defeat", army,
                     recipient);
    }
}

i32 SimState::count_alliance_components(
    const std::vector<i32>& army_indices) const {
    const size_t m = army_indices.size();
    if (m == 0) return 0;
    std::vector<char> seen(m, 0);
    i32 components = 0;
    std::vector<size_t> stack;
    for (size_t s = 0; s < m; ++s) {
        if (seen[s]) continue;
        ++components;
        seen[s] = 1;
        stack.push_back(s);
        while (!stack.empty()) {
            size_t cur = stack.back();
            stack.pop_back();
            for (size_t t = 0; t < m; ++t) {
                if (seen[t]) continue;
                // Alliances are set symmetrically; BFS over the ally graph
                // yields the transitive team even if only pairwise links exist.
                if (armies_[army_indices[cur]]->is_ally(army_indices[t])) {
                    seen[t] = 1;
                    stack.push_back(t);
                }
            }
        }
    }
    return components;
}

i32 SimState::surviving_team_count() const {
    std::vector<i32> alive;
    for (size_t i = 0; i < armies_.size(); ++i) {
        const auto& b = armies_[i];
        if (b->is_civilian() || b->is_defeated()) continue;
        alive.push_back(static_cast<i32>(i));
    }
    return count_alliance_components(alive);
}

void SimState::update_victory() {
    // The scripts' own victory check owns the outcome when it runs.
    if (script_victory_ || game_ended_ || victory_mode_ == VictoryMode::Sandbox) return;

    const size_t n = armies_.size();

    // Single registry pass: tally living units per army for each mode's
    // elimination criterion. Categories mirror FA's lua/victory.lua:
    //   demoralization = COMMAND
    //   domination     = STRUCTURE + ENGINEER - WALL
    //   eradication    = ALLUNITS - WALL
    struct Tally {
        i32 all = 0;              // every living unit (for the ever-had-units guard)
        i32 non_wall = 0;         // ALLUNITS - WALL (eradication)
        i32 struct_or_eng = 0;    // (STRUCTURE|ENGINEER) - WALL (domination)
        bool has_command = false; // a living, non-dying ACU (demoralization)
    };
    std::vector<Tally> tally(n);
    entity_registry_.for_each_unit([&](Entity& e) {
        if (!e.is_unit() || e.destroyed()) return;
        i32 a = e.army();
        if (a < 0 || a >= static_cast<i32>(n)) return;
        auto* u = static_cast<Unit*>(&e);
        Tally& t = tally[static_cast<size_t>(a)];
        ++t.all;
        if (!u->has_category("WALL")) {
            ++t.non_wall;
            if (u->has_category("STRUCTURE") || u->has_category("ENGINEER"))
                ++t.struct_or_eng;
        }
        // A dying unit is mid death-animation: treat the ACU as already lost so
        // Assassination resolves the instant the commander starts to die.
        if (u->has_category("COMMAND") && !u->is_dying())
            t.has_command = true;
    });

    for (size_t i = 0; i < n; ++i)
        if (tally[i].all > 0) armies_[i]->note_has_units();

    // Grace period: don't eliminate anyone until starting units have spawned.
    if (tick_count_ <= kVictoryGraceTicks) return;

    // A match needs at least two non-civilian participants to auto-resolve.
    i32 participants = 0;
    for (size_t i = 0; i < n; ++i)
        if (!armies_[i]->is_civilian()) ++participants;
    if (participants < 2) return;

    // Elimination pass (mode-specific). Only armies that have already fielded
    // units are eligible, so an army still loading isn't declared defeated.
    std::vector<i32> newly_defeated;
    for (size_t i = 0; i < n; ++i) {
        auto& b = armies_[i];
        if (b->is_civilian() || b->is_defeated() || !b->has_ever_had_units())
            continue;
        const Tally& t = tally[i];
        bool eliminated = false;
        switch (victory_mode_) {
        case VictoryMode::Demoralization: eliminated = !t.has_command; break;
        case VictoryMode::Domination:     eliminated = t.struct_or_eng == 0; break;
        case VictoryMode::Eradication:    eliminated = t.non_wall == 0; break;
        case VictoryMode::Sandbox:        eliminated = false; break;
        }
        if (eliminated) {
            b->set_state(BrainState::Defeat);
            newly_defeated.push_back(static_cast<i32>(i));
            spdlog::info("Army {} ({}) eliminated under '{}' victory condition",
                         i, b->name(), victory_condition_);
        }
    }

    // Dispose of each just-defeated army's remaining units per the share rule
    // (destroy, or transfer to an ally / civilian).
    for (i32 idx : newly_defeated) dispose_defeated_army(idx);

    // Determine surviving teams (alliance components of the still-alive armies).
    std::vector<i32> alive;
    for (size_t i = 0; i < n; ++i) {
        auto& b = armies_[i];
        if (b->is_civilian() || b->is_defeated()) continue;
        alive.push_back(static_cast<i32>(i));
    }
    const i32 teams = count_alliance_components(alive);

    if (teams >= 2) return; // game continues

    // Game over: one team (or none) remains.
    game_ended_ = true;
    if (teams == 1) {
        for (i32 idx : alive) {
            if (armies_[idx]->state() == BrainState::InProgress)
                armies_[idx]->set_state(BrainState::Victory);
        }
        spdlog::info("Game over: one team remains — victory declared");
    } else {
        // Every remaining combatant was eliminated on the same tick → draw.
        for (i32 idx : newly_defeated)
            armies_[idx]->set_state(BrainState::Draw);
        spdlog::info("Game over: mutual elimination — draw declared");
    }
}

void SimState::trace_rng_draw(void* ctx, u64 value) {
    auto& sim = *static_cast<SimState*>(ctx);
    std::string where = "engine";
    if (auto* L = static_cast<lua_State*>(sim.sim_random_.caller())) {
        where.clear();
        lua_Debug ar;
        for (int level = 1; level <= 4 && lua_getstack(L, level, &ar); ++level) {
            if (!lua_getinfo(L, "Sl", &ar)) break;
            if (!where.empty()) where += " < ";
            where += fmt::format("{}:{}", ar.short_src, ar.currentline);
        }
    }
    *sim.rng_trace_ << fmt::format("{} {} {:016x} {}\n", sim.tick_count_, sim.rng_trace_draws_++,
                                   value, where);
}

void SimState::write_entity_trace() const {
    const auto bits = [](f32 v) { return std::bit_cast<u32>(v); };
    std::string out = fmt::format("T {} rng {:016x}\n", tick_count_, sim_random_.digest());
    entity_registry_.for_each([&](const Entity& e) {
        const auto& p = e.position();
        const auto& q = e.orientation();
        const char kind = e.is_unit()         ? 'u'
                          : e.is_prop()       ? 'p'
                          : e.is_projectile() ? 'j'
                          : e.is_shield()     ? 's'
                                              : 'e';
        out += fmt::format("{} {} {} a{} d{} p {:08x} {:08x} {:08x} q {:08x} {:08x} {:08x} {:08x} "
                           "h {:08x}",
                           e.entity_id(), kind, e.blueprint_id(), e.army(), e.destroyed() ? 1 : 0,
                           bits(p.x), bits(p.y), bits(p.z), bits(q.x), bits(q.y), bits(q.z),
                           bits(q.w), bits(e.health()));
        if (e.is_unit()) {
            const auto& u = static_cast<const Unit&>(e);
            out += fmt::format(" L{} s{:x} f{} c{} k{} b{:08x}", u.layer(), u.script_bits(),
                               u.fire_state(), u.command_queue().size(), u.is_dying() ? 1 : 0,
                               bits(u.fraction_complete()));
        }
        out += '\n';
    });
    *entity_trace_ << out;
}

const SimState::ChecksumParts& SimState::beat_checksum() const {
    if (!beat_checksum_valid_ || beat_checksum_beat_ != beat_count_) {
        beat_checksum_ = checksum_parts();
        beat_checksum_beat_ = beat_count_;
        beat_checksum_valid_ = true;
    }
    return beat_checksum_;
}

u32 SimState::compute_sync_checksum() const {
    return checksum_parts().total();
}

i32 SimState::focus_army() const {
    if (!L_) {
        return 0;
    }
    lua_pushstring(L_, "__osc_focus_army");
    lua_rawget(L_, LUA_REGISTRYINDEX);
    const i32 army = lua_isnumber(L_, -1) ? static_cast<i32>(lua_tonumber(L_, -1)) : 0;
    lua_pop(L_, 1);
    return army;
}

i32 SimState::player_result() const {
    const i32 focus = focus_army();
    const ArmyBrain* player = focus >= 0 ? army_at(static_cast<size_t>(focus)) : nullptr;
    if (!player) return game_ended_ ? 3 : 0;

    // A decisive brain state set by update_victory (or by an external caller)
    // wins outright.
    if (player->state() == BrainState::Victory) return 1;
    if (player->state() == BrainState::Draw) return 3;

    const bool player_defeated = player->is_defeated();

    // Are all of the player's non-civilian, non-allied enemies defeated? This
    // inference also covers callers that set brain states directly without
    // ticking through update_victory (e.g. the --draw-test / --full-smoke-test
    // harnesses).
    bool all_enemies_dead = true;
    bool has_enemy = false;
    for (size_t i = 0; i < army_count(); ++i) {
        const auto* b = army_at(i);
        if (!b || b->is_civilian() || static_cast<i32>(i) == focus) {
            continue;
        }
        if (player->is_ally(static_cast<i32>(i))) {
            continue;
        }
        has_enemy = true;
        if (!b->is_defeated()) {
            all_enemies_dead = false;
            break;
        }
    }

    // Player and all enemies defeated → draw; player alone defeated → loss.
    if (player_defeated && all_enemies_dead && has_enemy) return 3;
    if (player_defeated) return 2;
    // All enemies defeated, player still standing → victory.
    if (all_enemies_dead && has_enemy) return 1;

    // Undecided: report a draw only once the game has otherwise ended (e.g. the
    // player is an observer / civilian), else still in progress.
    return game_ended_ ? 3 : 0;
}

} // namespace osc::sim
