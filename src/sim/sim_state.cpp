#include "sim/sim_state.hpp"
#include "sim/blueprint_categories.hpp"
#include "sim/collision_beam.hpp"
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
#include "map/visibility_grid.hpp"
#include "sim/entity.hpp"
#include "sim/projectile.hpp"
#include "sim/prop.hpp"
#include "sim/shield.hpp"
#include "sim/unit.hpp"

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

#include <spdlog/spdlog.h>

#include <algorithm>
#include <unordered_map>
#include <bit>
#include <ostream>
#include <string>
#include <cmath>
#include <cctype>
#include <cstring>
#include <utility>
#include <vector>

namespace osc::sim {

u32 SimState::s_sim_generation_ = 0;

SimState::SimState(lua_State* L, blueprints::BlueprintStore* store)
    : L_(L), thread_manager_(L), blueprint_store_(store) {
    ++s_sim_generation_;
    // Sim code (e.g. weapons) draws randomness from this seeded stream via the
    // registry, so every lockstep client rolls identically.
    entity_registry_.set_sim_random(&sim_random_);
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

void SimState::on_entity_unregistered(Entity& entity) {
    // Removed by the engine (impact, reclaim, crash...) rather than by a
    // script's Destroy(): the script's OnDestroy still runs, first.
    notify_script_destroy(entity);
    // Its ambient loops end with it (the sound engine outlives the sim).
    for (const auto& a : entity.take_ambient_sounds())
        if (sound_manager_) sound_manager_->stop(a.handle, false);

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
        // A stored unit leaves its carrier's storage; a carrier's stored
        // units go with it (Moho's ~CAiTransportImpl), destroyed once this
        // unregistration is over (M206q).
        if (unit.transport_id() != 0)
            if (Entity* c = entity_registry_.find(unit.transport_id()); c && c->is_unit())
                static_cast<Unit*>(c)->forget_stored(unit.entity_id());
        for (const u32 id : unit.stored_ids()) stored_to_destroy_.push_back(id);
    }

    // The Lua table outlives the C++ object: null its _c_object so methods
    // called on a stale handle see "destroyed" instead of freed memory, and
    // release the registry reference. entity_Destroy already does this for
    // Lua-initiated destruction; reclaim, sacrifice and projectile impact
    // unregister directly from C++ and relied on this path.
    const int ref = entity.lua_table_ref();
    if (L_ && ref >= 0) {
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
            e->mark_destroyed();
            entity_registry_.unregister_entity(id);
        }
    }
}

SimState::~SimState() {
    // The sound engine outlives the sim: the sim's loops stop with it.
    if (sound_manager_) {
        entity_registry_.for_each([&](const Entity& e) {
            for (const auto& a : e.ambient_sounds()) sound_manager_->stop(a.handle);
        });
    }
    // The sim Lua state may outlive this sim; it must not keep reaching the
    // sound engine through it.
    if (L_ && sound_manager_) {
        lua_pushstring(L_, "osc_sound_manager");
        lua_pushnil(L_);
        lua_rawset(L_, LUA_REGISTRYINDEX);
    }
}

void SimState::reap_empty_platoons() {
    if (!L_) return;
    for (const auto& army : armies_) {
        // In creation order; OnDestroy may make platoons, which come after.
        for (size_t i = 0; i < army->platoon_count(); ++i) {
            Platoon* p = army->platoon_at(i);
            if (!p || p->destroyed() || !p->had_units() || p->name() == "ArmyPool") continue;
            const bool manned =
                std::any_of(p->unit_ids().begin(), p->unit_ids().end(), [this](u32 id) {
                    const Entity* e = entity_registry_.find(id);
                    return e && !e->destroyed();
                });
            if (manned) continue;
            army->destroy_platoon(p);
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

void SimState::follow_attachments() {
    // The parent's pose is read during the walk and applied after it: a
    // chain (A on B on C) then lags one tick per link whatever the
    // registry's iteration order, as lockstep needs.
    //
    // A child jumps (Entity::note_snap) on its first follow -- attaching is
    // a jump onto the parent -- and whenever its parent has jumped since, so
    // the renderer pops attachments along with a teleport, link by link.
    struct Move {
        Entity* child;
        Vector3 pos;
        Quaternion orient;
        u32 parent_snap;
        bool snap;
    };
    std::vector<Move> moves;
    entity_registry_.for_each([&](const Entity& e) {
        if (e.parent_entity_id() == 0 || e.destroyed()) return;
        const Entity* parent = entity_registry_.find(e.parent_entity_id());
        if (!parent || parent->destroyed()) return;
        const Vector3& p = parent->position();
        const Quaternion& q = parent->orientation();
        const Vector3& c = e.position();
        const Quaternion& o = e.orientation();
        const bool snap = e.followed_parent_snap() != parent->snap_serial();
        if (snap || c.x != p.x || c.y != p.y || c.z != p.z || o.x != q.x || o.y != q.y ||
            o.z != q.z || o.w != q.w)
            moves.push_back({const_cast<Entity*>(&e), p, q, parent->snap_serial(), snap});
    });
    // Applied after the walk: set_position updates the spatial grid.
    for (const auto& m : moves) {
        m.child->set_position(m.pos);
        m.child->set_orientation(m.orient);
        if (m.snap) {
            m.child->note_snap();
            m.child->set_followed_parent_snap(m.parent_snap);
        }
    }
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
}

void SimState::set_no_rush(f32 seconds, f32 radius) {
    no_rush_seconds_ = seconds > 0.0f ? seconds : 0.0f;
    if (radius > 0.0f) no_rush_radius_ = radius;
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
    terrain_ = std::move(terrain);
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

void SimState::build_visibility_grid() {
    if (!terrain_) return;
    visibility_grid_ = std::make_unique<map::VisibilityGrid>(
        terrain_->map_width(), terrain_->map_height());
    visibility_grid_->build_height_grid(*terrain_);
    spdlog::info("Built visibility grid: {}x{} cells (cell_size={})",
                 visibility_grid_->grid_width(),
                 visibility_grid_->grid_height(),
                 visibility_grid_->cell_size());
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

// --- Stealth-aware intel query helpers ---

bool SimState::has_effective_radar(const Entity* entity,
                                    u32 req_army) const {
    if (!visibility_grid_ || !entity) return false;
    auto& pos = entity->position();
    if (!visibility_grid_->has_radar(pos.x, pos.z, req_army)) return false;
    // RadarStealth negates radar unless observer has Omni
    if (entity->is_unit()) {
        auto* unit = static_cast<const Unit*>(entity);
        if (unit->is_intel_enabled("RadarStealth") &&
            !visibility_grid_->has_omni(pos.x, pos.z, req_army))
            return false;
    }
    return true;
}

bool SimState::has_effective_sonar(const Entity* entity,
                                    u32 req_army) const {
    if (!visibility_grid_ || !entity) return false;
    auto& pos = entity->position();
    if (!visibility_grid_->has_sonar(pos.x, pos.z, req_army)) return false;
    // SonarStealth negates sonar unless observer has Omni
    if (entity->is_unit()) {
        auto* unit = static_cast<const Unit*>(entity);
        if (unit->is_intel_enabled("SonarStealth") &&
            !visibility_grid_->has_omni(pos.x, pos.z, req_army))
            return false;
    }
    return true;
}

bool SimState::has_any_intel(const Entity* entity, u32 req_army) const {
    if (!visibility_grid_ || !entity) return false;
    auto& pos = entity->position();
    bool omni = visibility_grid_->has_omni(pos.x, pos.z, req_army);
    bool cloaked = entity->is_unit() &&
                   static_cast<const Unit*>(entity)->is_cloaked();
    bool vision = visibility_grid_->has_vision(pos.x, pos.z, req_army) &&
                  (!cloaked || omni);
    return vision ||
           has_effective_radar(entity, req_army) ||
           has_effective_sonar(entity, req_army) ||
           omni;
}

// --- Cached stealth variants (avoid per-army is_intel_enabled string lookups) ---

bool SimState::has_effective_radar_cached(const Entity* entity, u32 req_army,
                                           bool radar_stealth) const {
    if (!visibility_grid_ || !entity) return false;
    auto& pos = entity->position();
    if (!visibility_grid_->has_radar(pos.x, pos.z, req_army)) return false;
    if (radar_stealth && !visibility_grid_->has_omni(pos.x, pos.z, req_army))
        return false;
    return true;
}

bool SimState::has_effective_sonar_cached(const Entity* entity, u32 req_army,
                                           bool sonar_stealth) const {
    if (!visibility_grid_ || !entity) return false;
    auto& pos = entity->position();
    if (!visibility_grid_->has_sonar(pos.x, pos.z, req_army)) return false;
    if (sonar_stealth && !visibility_grid_->has_omni(pos.x, pos.z, req_army))
        return false;
    return true;
}

bool SimState::has_any_intel_cached(const Entity* entity, u32 req_army,
                                     bool radar_stealth,
                                     bool sonar_stealth,
                                     bool cloaked) const {
    if (!visibility_grid_ || !entity) return false;
    auto& pos = entity->position();
    bool omni = visibility_grid_->has_omni(pos.x, pos.z, req_army);
    bool vision = visibility_grid_->has_vision(pos.x, pos.z, req_army) &&
                  (!cloaked || omni);
    return vision ||
           has_effective_radar_cached(entity, req_army, radar_stealth) ||
           has_effective_sonar_cached(entity, req_army, sonar_stealth) ||
           omni;
}

u32 SimState::schedule_command(u32 source, const std::vector<u32>& unit_ids,
                               const UnitCommand& command, bool clear_existing) {
    const u32 exec_tick = tick_count_ + 1 + command_delay_;
    ScheduledCommand sc;
    sc.exec_tick = exec_tick;
    sc.source = source;
    sc.command = command;
    sc.unit_ids = unit_ids;
    sc.clear_existing = clear_existing;
    command_scheduler_.submit(std::move(sc));
    return exec_tick;
}

u32 SimState::schedule_callback(u32 source, SimCallbackEntry callback) {
    ScheduledCommand sc;
    sc.exec_tick = tick_count_ + 1 + command_delay_;
    sc.source = source;
    sc.callback = std::move(callback);
    const u32 exec_tick = sc.exec_tick;
    command_scheduler_.submit(std::move(sc));
    return exec_tick;
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
    // A player's order is a command, applied inside a tick as Moho applies
    // it: under a network session it is broadcast and every peer schedules it
    // for the same tick; in single-player it is scheduled for the next tick.
    // Either way a replay can record it.
    if (human_input_active_) {
        if (playback_) return 0; // a replay plays only what it recorded
        if (local_command_sink_) local_command_sink_(unit_ids, command, clear_existing);
        else schedule_command(0, unit_ids, command, clear_existing);
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
    bool queued = false;
    for (const auto& [uid, cmd] : expand_group_command(unit_ids, issued)) {
        auto* e = entity_registry_.find(uid);
        if (!e || e->destroyed() || !e->is_unit()) continue;
        auto* unit = static_cast<Unit*>(e);
        // Stop clears the queue outright (rather than queueing a Stop order), so
        // it matches the old IssueStop's immediate clear_commands() semantics.
        if (cmd.type == CommandType::Stop) {
            stop_unit(*unit);
        } else if (!apply_silo_build(*unit, cmd)) {
            unit->push_command(cmd, clear_existing);
            queued = true;
        }
    }
    return queued ? issued.command_id : 0;
}

bool SimState::command_queued(u32 command_id) const {
    if (command_id == 0) return false;
    bool found = false;
    entity_registry_.for_each_unit([&](const Entity& e) {
        if (found || e.destroyed()) return;
        for (const auto& c : static_cast<const Unit&>(e).command_queue())
            if (c.command_id == command_id) {
                found = true;
                return;
            }
    });
    return found;
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
    if (!command.formation.empty() && command.type == CommandType::Move) {
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
                cmd.speed_cap = pace;
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
    recorded_replay_.final_tick = tick_count_;
    recorded_replay_.checksum_from = tick_count_ + 1;
}

void SimState::set_game_setup(GameSetup setup) {
    recorded_replay_.setup = std::move(setup);
    recorded_replay_.has_setup = true;
}

void SimState::queue_replay(const Replay& replay) {
    command_delay_ = replay.command_delay;
    if (!replay.victory_condition.empty())
        set_victory_condition(replay.victory_condition);
    for (const auto& c : replay.commands) command_scheduler_.submit(c);
}

void SimState::start_resume(const Replay& saved) {
    queue_replay(saved);
    const bool behind = saved.final_tick > tick_count_;
    playback_ = behind;
    resume_tick_ = behind ? saved.final_tick : 0;
}

std::shared_ptr<const ProjectileBlueprintInfo>
SimState::projectile_blueprint_info(const std::string& bp_id) {
    if (auto it = projectile_info_.find(bp_id); it != projectile_info_.end()) return it->second;
    auto info = std::make_shared<ProjectileBlueprintInfo>();
    info->categories.insert("ALLPROJECTILES");
    if (L_ && !bp_id.empty()) {
        const int top = lua_gettop(L_);
        lua_pushstring(L_, "__blueprints");
        lua_rawget(L_, LUA_GLOBALSINDEX);
        if (lua_istable(L_, -1)) {
            lua_pushstring(L_, bp_id.c_str());
            lua_rawget(L_, -2);
        }
        if (lua_istable(L_, -1)) {
            const int bp = lua_gettop(L_);
            collect_blueprint_categories(L_, bp, info->categories);
            lua_pushstring(L_, "DesiredShooterCap");
            lua_rawget(L_, bp);
            if (lua_type(L_, -1) == LUA_TNUMBER && lua_tonumber(L_, -1) > 0)
                info->desired_shooter_cap = static_cast<u32>(lua_tonumber(L_, -1));
        }
        lua_settop(L_, top);
    }
    projectile_info_.emplace(bp_id, info);
    return info;
}

void SimState::stop_unit(Unit& unit) {
    const bool factory_build = unit.building_factory_order();
    unit.clear_commands();
    // The order it was working on goes too: a factory's unit under
    // construction, or an enhancement under way.
    if (factory_build) unit.cancel_factory_build(entity_registry_, L_);
    if (!unit.destroyed() && unit.is_enhancing()) unit.cancel_enhance(L_);
}

void SimState::dispatch_due_commands() {
    PROFILE_ZONE("Sim::commands");
    const bool no_rush = no_rush_active();
    command_scheduler_.dispatch_due(tick_count_, [&](const ScheduledCommand& scheduled) {
        // A player's order, or UI callback, moves only its own army's units.
        ScheduledCommand sc = scheduled;
        if (const auto owner = source_armies_.find(sc.source); owner != source_armies_.end()) {
            const auto own_only = [&](std::vector<u32>& ids) {
                const size_t named = ids.size();
                ids.erase(std::remove_if(ids.begin(), ids.end(),
                                         [&](u32 id) {
                                             const Entity* e = entity_registry_.find(id);
                                             return !e || e->army() != owner->second;
                                         }),
                          ids.end());
                if (ids.size() != named)
                    spdlog::warn("[commands] source {} named {} units outside its army {}; "
                                 "ignored",
                                 sc.source, named - ids.size(), owner->second);
            };
            own_only(sc.unit_ids);
            if (sc.callback) own_only(sc.callback->unit_ids);
        }
        // A recording keeps what the sim applies, where it applies it: local
        // and networked commands alike, on the tick they ran.
        if (recording_) {
            recorded_replay_.commands.push_back(sc);
            recorded_replay_.commands.back().exec_tick = tick_count_;
        }
        if (sc.callback) {
            run_sim_callback(*sc.callback);
            return;
        }
        // Command ids come from the sim's counter here, inside the tick, so
        // every peer (and a replay) numbers an order the same way; the id it
        // arrived with was the issuer's.
        UnitCommand base = sc.command;
        base.command_id = next_command_id();
        if (base.factory) {
            apply_factory_command(sc.unit_ids, base, sc.clear_existing);
            return;
        }
        for (auto& [uid, expanded] : expand_group_command(sc.unit_ids, base)) {
            auto* e = entity_registry_.find(uid);
            if (!e || e->destroyed() || !e->is_unit()) continue;
            auto* unit = static_cast<Unit*>(e);
            // A scheduled Stop clears the queue (mirrors route_command's direct
            // branch), so a networked player's Stop lands identically on peers.
            if (sc.command.type == CommandType::Stop) {
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
            unit->push_command(cmd, sc.clear_existing);
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
        const Vector3& p = unit->position();
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
    tick_count_++;
    game_time_ = tick_count_ * SECONDS_PER_TICK;
    if (rng_trace_) {
        const bool on = tick_count_ >= rng_trace_from_ && tick_count_ <= rng_trace_to_;
        sim_random_.set_draw_hook(on ? &SimState::trace_rng_draw : nullptr, this);
    }

    // Apply the commands scheduled for this tick before anything simulates,
    // so orders take effect deterministically at the start of the frame.
    dispatch_due_commands();
    update_influence_maps();

    if (pathfinder_) {
        pathfinder_->reset_request_count();
    }

    {
        PROFILE_ZONE("Sim::threads");
        thread_manager_.resume_all(tick_count_);
    }

    request_economy_events();
    update_economies();
    update_entities();
    // Beams reach from where their muzzles have moved to (M206c).
    update_collision_beams(*this, L_);
    sweep_ferry_beacons();

    // Aircraft killed in flight that landed this tick: Moho tells their
    // script, whose OnImpact deals the DeathImpact weapon's damage and plays
    // the death out (or sinks it, in water). One without a script just goes.
    {
        std::vector<u32> landed;
        entity_registry_.for_each_unit([&](Entity& e) {
            if (!e.destroyed() && static_cast<Unit&>(e).take_crash_impact())
                landed.push_back(e.entity_id());
        });
        for (const u32 id : landed) {
            Entity* e = entity_registry_.find(id);
            if (!e || e->destroyed()) continue;
            if (!L_ || e->lua_table_ref() < 0) {
                e->mark_destroyed();
                entity_registry_.unregister_entity(id);
                continue;
            }
            const Vector3 p = e->position();
            const bool water = terrain_ && terrain_->has_water() &&
                               terrain_->get_terrain_height(p.x, p.z) < terrain_->water_elevation();
            // On the ground it is a land unit now: its wreck follows the
            // blueprint's WreckageLayers.Land.
            if (!water) {
                static_cast<Unit*>(e)->set_layer_with_callback("Land", L_);
                e = entity_registry_.find(id);
                if (!e || e->destroyed() || e->lua_table_ref() < 0) continue;
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
                    const std::string message =
                        std::string("OnImpact error: ") + (err ? err : "(unknown)");
                    spdlog::warn("{}", message);
                    if (test_status::count_lua_failures()) test_status::record_failure(message);
                }
            }
            lua_settop(L_, top);
            // Nothing will play this death out (no OnImpact, or it broke):
            // finish it, as Kill does when OnKilled fails, rather than leave
            // a dead unit lying there forever.
            e = entity_registry_.find(id);
            if (!handled && e && !e->destroyed()) {
                if (e->lua_table_ref() >= 0) {
                    lua_rawgeti(L_, LUA_REGISTRYINDEX, e->lua_table_ref());
                    lua_pushstring(L_, "Destroy");
                    lua_gettable(L_, -2);
                    if (lua_isfunction(L_, -1)) {
                        lua_pushvalue(L_, top + 1);
                        if (lua_pcall(L_, 1, 0, 0) != 0)
                            spdlog::warn("Destroy error: {}", lua_tostring(L_, -1));
                    }
                    lua_settop(L_, top);
                }
                e = entity_registry_.find(id);
                if (e && !e->destroyed()) {
                    e->mark_destroyed();
                    entity_registry_.unregister_entity(id);
                }
            }
        }
    }

    // "No Rush": pin units that strayed beyond their confinement radius.
    enforce_no_rush();

    reap_empty_platoons();

    update_visibility();
    feed_influence_map();

    // --- Victory-condition enforcement (mode + team aware) ---
    update_victory();

    follow_attachments();

    if (sound_manager_) {
        PROFILE_ZONE("Sim::audio");
        // Ambient loops follow their entities.
        entity_registry_.for_each([&](const Entity& e) {
            for (const auto& a : e.ambient_sounds()) sound_manager_->set_position(a.handle, e.position());
        });
        // A headless run has no frames, so the sim tick is its clock.
        if (sound_manager_->sim_clocked()) sound_manager_->update(0.1f);
    }

    // Economy events: tick drains, wake waiting threads on completion
    {
        PROFILE_ZONE("Sim::econ_events");
        tick_economy_events();
    }

    destroy_orphaned_stored_units();

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
    if (tick_count_ % LUA_GC_PERIOD_TICKS == 0) {
        PROFILE_ZONE("Sim::lua_gc");
        lua_setgcthreshold(L_, 0);
    }

    // Entities unregistered this tick may still have been on the C++ stack
    // (destroyed from their own callbacks); only now is freeing them safe.
    entity_registry_.collect_garbage();

    if (tick_observer_) {
        PROFILE_ZONE("Sim::observer");
        tick_observer_(*this);
    }
    if (checksum_trace_ || recording_) {
        const ChecksumParts& parts = tick_checksum();
        if (checksum_trace_) {
            if (!checksum_trace_header_) {
                std::string header = "# tick total";
                for (const char* name : ChecksumParts::kNames) header += fmt::format(" {}", name);
                *checksum_trace_ << header << '\n';
                checksum_trace_header_ = true;
            }
            std::string line = fmt::format("{} {:08x}", tick_count_, parts.total());
            for (const u64 part : parts.values()) line += fmt::format(" {:016x}", part);
            *checksum_trace_ << line << '\n';
        }
        if (recording_) {
            recorded_replay_.final_tick = tick_count_;
            recorded_replay_.checksums.push_back(parts.total());
        }
    }
    if (entity_trace_ && tick_count_ >= entity_trace_from_ && tick_count_ <= entity_trace_to_)
        write_entity_trace();
    // Death flashes and camera shakes are shown from the tick's capture;
    // the sim is done with them (events raised between ticks wait for the
    // next one).
    death_events_.clear();
    camera_shake_events_.clear();
    // A loaded game has caught up: the player's orders count from here.
    if (resume_tick_ != 0 && tick_count_ >= resume_tick_) {
        resume_tick_ = 0;
        playback_ = false;
    }
}

void SimState::update_economies() {
    PROFILE_ZONE("Sim::economy");
    for (auto& army : armies_) {
        army->update_economy(entity_registry_, SECONDS_PER_TICK);
    }
    share_team_economy();
}

std::vector<std::vector<i32>> SimState::alliance_teams() const {
    std::vector<i32> members;
    for (size_t i = 0; i < armies_.size(); ++i) {
        if (!armies_[i]->is_civilian()) members.push_back(static_cast<i32>(i));
    }
    const size_t m = members.size();
    std::vector<char> seen(m, 0);
    std::vector<std::vector<i32>> teams;
    std::vector<size_t> stack;
    for (size_t s = 0; s < m; ++s) {
        if (seen[s]) continue;
        std::vector<i32> team;
        seen[s] = 1;
        stack.push_back(s);
        while (!stack.empty()) {
            size_t cur = stack.back();
            stack.pop_back();
            team.push_back(members[cur]);
            for (size_t t = 0; t < m; ++t) {
                if (seen[t]) continue;
                if (armies_[members[cur]]->is_ally(members[t])) {
                    seen[t] = 1;
                    stack.push_back(t);
                }
            }
        }
        teams.push_back(std::move(team));
    }
    return teams;
}

namespace {
// Distribute a pooled amount of a resource across active members proportional
// to each member's remaining storage room (capped by that room).
void distribute_overflow(std::vector<ResourceState*>& res, f64 pool) {
    if (pool <= 0.0) return;
    f64 room_total = 0.0;
    for (auto* r : res) room_total += std::max(0.0, r->max_storage - r->stored);
    if (room_total <= 0.0) return;
    f64 give = std::min(pool, room_total);
    for (auto* r : res) {
        f64 room = std::max(0.0, r->max_storage - r->stored);
        r->stored = std::min(r->max_storage, r->stored + give * (room / room_total));
    }
}
} // namespace

void SimState::share_team_economy() {
    if (!common_army_ && !team_share_overflow_) return;
    PROFILE_ZONE("Sim::share_econ");
    for (const auto& team : alliance_teams()) {
        if (team.size() < 2) continue;
        // Active (non-defeated) members only, so a dead ally's reserves aren't
        // resurrected and don't receive shares.
        std::vector<ArmyBrain*> members;
        for (i32 idx : team) {
            auto* b = armies_[idx].get();
            if (!b->is_defeated()) members.push_back(b);
        }
        if (members.size() < 2) continue;

        if (common_army_) {
            // Full pool: every member ends at the same storage fill ratio.
            f64 mass_pool = 0.0, mass_cap = 0.0;
            f64 energy_pool = 0.0, energy_cap = 0.0;
            for (auto* b : members) {
                mass_pool += b->economy().mass.stored;
                mass_cap += b->economy().mass.max_storage;
                energy_pool += b->economy().energy.stored;
                energy_cap += b->economy().energy.max_storage;
            }
            for (auto* b : members) {
                auto& econ = b->economy();
                if (mass_cap > 0.0)
                    econ.mass.stored = mass_pool * (econ.mass.max_storage / mass_cap);
                if (energy_cap > 0.0)
                    econ.energy.stored = energy_pool * (econ.energy.max_storage / energy_cap);
            }
        } else {
            // Overflow only: resources members would waste at full storage flow
            // to allies with room.
            f64 mass_overflow = 0.0, energy_overflow = 0.0;
            std::vector<ResourceState*> mass_res, energy_res;
            for (auto* b : members) {
                mass_overflow += b->economy().mass.overflow;
                energy_overflow += b->economy().energy.overflow;
                mass_res.push_back(&b->economy().mass);
                energy_res.push_back(&b->economy().energy);
            }
            distribute_overflow(mass_res, mass_overflow);
            distribute_overflow(energy_res, energy_overflow);
        }
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

void SimState::request_economy_events() {
    // Each event asks its unit's army for its cost over its duration; one
    // whose unit is gone is cancelled.
    economy_events_.for_each([&](EconomyEvent& evt) {
        evt.set_counted(false);
        if (!evt.active() || evt.unit_id() == 0) return;
        const Entity* unit = entity_registry_.find(evt.unit_id());
        if (!unit || unit->destroyed()) {
            evt.cancel();
            return;
        }
        if (ArmyBrain* brain = get_army(unit->army())) {
            brain->add_event_request(evt.mass_per_second(), evt.energy_per_second());
            evt.set_counted(true);
        }
    });
}

void SimState::tick_economy_events() {
    // Events counted in this tick's economy move on as it granted them (a
    // stall slows them), and tell their script how far they are. One made
    // since asks next tick. An event with no unit runs on time alone.
    economy_events_.for_each([&](EconomyEvent& evt) {
        if (!evt.active()) return;
        f64 efficiency = 1.0;
        u32 unit_id = evt.unit_id();
        if (unit_id != 0) {
            if (!evt.counted()) return;
            const Entity* unit = entity_registry_.find(unit_id);
            const ArmyBrain* brain = unit ? get_army(unit->army()) : nullptr;
            if (brain) {
                if (evt.mass_per_second() > 0)
                    efficiency = std::min(efficiency, brain->mass_efficiency());
                if (evt.energy_per_second() > 0)
                    efficiency = std::min(efficiency, brain->energy_efficiency());
            }
        }
        evt.advance(SECONDS_PER_TICK, efficiency);
        if (evt.callback_ref() < 0 || unit_id == 0) return;
        const Entity* unit = entity_registry_.find(unit_id);
        if (!unit || unit->destroyed() || unit->lua_table_ref() < 0) return;
        const int top = lua_gettop(L_);
        lua_rawgeti(L_, LUA_REGISTRYINDEX, evt.callback_ref());
        if (lua_isfunction(L_, -1)) {
            lua_rawgeti(L_, LUA_REGISTRYINDEX, unit->lua_table_ref());
            lua_pushnumber(L_, evt.progress());
            if (lua_pcall(L_, 2, 0, 0) != 0) {
                const char* err = lua_tostring(L_, -1);
                const std::string message =
                    std::string("EconomyEvent callback error: ") + (err ? err : "(unknown)");
                spdlog::warn("{}", message);
                if (test_status::count_lua_failures()) test_status::record_failure(message);
            }
        }
        lua_settop(L_, top);
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

void SimState::update_entities() {
    PROFILE_ZONE("Sim::entities");
    // Snapshot IDs to avoid iterator invalidation if update() triggers removal
    std::vector<u32> ids;
    ids.reserve(entity_registry_.count());
    entity_registry_.for_each([&](Entity& e) {
        ids.push_back(e.entity_id());
    });

    SimContext ctx{entity_registry_, L_, terrain_.get(),
                   pathfinder_.get(), pathfinding_grid_.get(),
                   visibility_grid_.get(), this, {}};
    for (size_t i = 0; i < armies_.size() && i < SimContext::MAX_EFFICIENCY_ARMIES; ++i) {
        ctx.army_efficiency[i] = {armies_[i]->mass_efficiency(),
                                  armies_[i]->energy_efficiency()};
    }

    for (u32 id : ids) {
        auto* e = entity_registry_.find(id);
        if (!e || e->destroyed()) continue;
        if (e->is_unit()) {
            const Vector3 before = e->position();
            const u32 snaps = e->snap_serial();
            static_cast<Unit*>(e)->update(SECONDS_PER_TICK, ctx);
            if (auto* moved = entity_registry_.find(id); moved && !moved->destroyed()) {
                // A teleport or a boarding jumps: no speed to lead by.
                const Vector3 after = moved->position();
                const auto per_second = static_cast<f32>(1.0 / SECONDS_PER_TICK);
                static_cast<Unit*>(moved)->set_velocity(
                    moved->snap_serial() != snaps ? Vector3{}
                                                  : Vector3{(after.x - before.x) * per_second,
                                                            (after.y - before.y) * per_second,
                                                            (after.z - before.z) * per_second});
            }
        } else if (e->is_projectile()) {
            static_cast<Projectile*>(e)->update(SECONDS_PER_TICK,
                                                 entity_registry_, L_, terrain_.get());
        } else if (e->is_prop()) {
            // A fallen tree sinking away (SinkAway) before its script destroys it.
            auto* prop = static_cast<Prop*>(e);
            if (prop->sink_rate != 0) {
                Vector3 p = prop->position();
                p.y += prop->sink_rate * static_cast<f32>(SECONDS_PER_TICK);
                prop->set_position(p);
            }
        }
    }
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
        Vector3 p = u.position();
        p.x += push_x[i];
        p.z += push_z[i];
        if (pathfinding_grid_) {
            u32 gx = 0, gz = 0;
            pathfinding_grid_->world_to_grid(p.x, p.z, gx, gz);
            if (!pathfinding_grid_->is_passable_for(gx, gz, u.layer(), u.naval_draft(),
                                                    u.is_amphibious() || u.is_hover()))
                continue;
        }
        // On the surface as it drives; a submarine keeps its depth.
        if (terrain_ && !bodies[i].sub) p.y = terrain_->get_surface_height(p.x, p.z);
        u.set_position(clamp_to_playable(p));
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
    if (!visibility_grid_) return;
    const u32 n = static_cast<u32>(
        std::min(army_count(), static_cast<size_t>(map::VisibilityGrid::MAX_ARMIES)));
    if (n == 0) return;
    // Moho's recon ticks one army a tick, in turn.
    const u32 a = tick_count_ % n;
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
        const bool detected =
            is_ally(owner, u.army()) || has_any_intel_cached(&e, a, u.has_radar_stealth(),
                                                             u.has_sonar_stealth(), u.is_cloaked());
        if (detected || (!moves(u) && ever_in_sight(e.entity_id(), a)))
            map->report(e.entity_id(), u.army(), u.position(), threat_source_of(u));
    });

    // A dead structure's entry goes once the army sees where it stood, or
    // it was an ally's; a mobile unit's fades out instead.
    std::vector<u32> gone;
    map->for_each_entry([&](const InfluenceMap::EntryView& v) {
        if (v.mobile || alive(entity_registry_.find(v.id))) return;
        if (is_ally(owner, v.source_army) ||
            visibility_grid_->has_vision(v.position.x, v.position.z, a))
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
        map->update([&](i32 army) { return army == owner || is_ally(owner, army); },
                    [&](u32 id) -> std::optional<InfluenceMap::UnitState> {
                        const Entity* e = entity_registry_.find(id);
                        if (!alive(e)) return std::nullopt;
                        const auto& u = static_cast<const Unit&>(*e);
                        InfluenceMap::UnitState s;
                        const std::string& layer = u.layer();
                        if (layer == "Land") s.layer = ThreatLayer::Land;
                        else if (layer == "Water" || layer == "Seabed" || layer == "Sub")
                            s.layer = ThreatLayer::Naval;
                        s.detailed = ever_in_sight(id, static_cast<u32>(owner)) ||
                                     (visibility_grid_ && i < map::VisibilityGrid::MAX_ARMIES &&
                                      visibility_grid_->has_omni(u.position().x, u.position().z,
                                                                 static_cast<u32>(owner)));
                        return s;
                    });
    }
}

void SimState::update_visibility() {
    PROFILE_ZONE("Sim::visibility");
    if (!visibility_grid_) return;

    // 1. Clear transient flags (keep EverSeen)
    visibility_grid_->clear_transient();

    // "No Fog of War": reveal the whole map to every army, then let the normal
    // painting run on top (harmlessly). Intel queries then see everything.
    if (fog_mode_ == FogMode::None) {
        u32 fn = static_cast<u32>(
            std::min(army_count(),
                     static_cast<size_t>(map::VisibilityGrid::MAX_ARMIES)));
        for (u32 a = 0; a < fn; ++a) visibility_grid_->reveal_all(a);
    }

    // 2. Paint intel radii: a source's radius for each intel type it has
    // switched on (0 = none).
    const auto paint_intel = [&](u32 ua, const Vector3& pos, const auto& radius_of) {
        // Vision: terrain LOS occlusion
        if (const f32 r = radius_of("Vision"); r > 0.0f) {
            f32 eye_h =
                terrain_->get_terrain_height(pos.x, pos.z) + map::VisibilityGrid::EYE_OFFSET;
            visibility_grid_->paint_circle_los(ua, pos.x, pos.z, r, eye_h);
        }
        // WaterVision maps to Vision flag but no terrain LOS (underwater sensing)
        if (const f32 r = radius_of("WaterVision"); r > 0.0f)
            visibility_grid_->paint_circle(ua, pos.x, pos.z, r, map::VisFlag::Vision);
        // Radar/Sonar/Omni: simple circle (not blocked by terrain)
        struct IntelMapping {
            const char* type;
            map::VisFlag flag;
        };
        static const IntelMapping non_los[] = {
            {"Radar", map::VisFlag::Radar},
            {"Sonar", map::VisFlag::Sonar},
            {"Omni", map::VisFlag::Omni},
        };
        for (const auto& m : non_los)
            if (const f32 r = radius_of(m.type); r > 0.0f)
                visibility_grid_->paint_circle(ua, pos.x, pos.z, r, m.flag);
    };
    const auto valid_army = [](i32 army) {
        return army >= 0 && army < static_cast<i32>(map::VisibilityGrid::MAX_ARMIES);
    };

    entity_registry_.for_each_unit([&](Entity& e) {
        if (e.destroyed() || !e.is_unit()) return;
        auto* unit = static_cast<Unit*>(&e);
        if (!valid_army(unit->army())) return;
        const auto& pos = unit->position();
        const u32 ua = static_cast<u32>(unit->army());
        paint_intel(ua, pos, [&](const char* type) {
            return unit->is_intel_enabled(type) ? unit->get_intel_radius(type) : 0.0f;
        });

        // Self-vision: own army always sees own unit cell
        visibility_grid_->paint_circle(
            ua, pos.x, pos.z,
            static_cast<f32>(map::VisibilityGrid::CELL_SIZE) * 0.5f,
            map::VisFlag::Vision);
    });

    // Script entities' intel (VizMarkers), dropped once the entity is gone.
    for (auto it = entity_intel_.begin(); it != entity_intel_.end();) {
        const Entity* e = entity_registry_.find(it->first);
        if (!e || e->destroyed()) {
            it = entity_intel_.erase(it);
            continue;
        }
        if (valid_army(it->second.army)) {
            const auto& sources = it->second.sources;
            paint_intel(static_cast<u32>(it->second.army), e->position(), [&](const char* type) {
                const auto s = sources.find(type);
                return s != sources.end() && s->second.enabled ? s->second.radius : 0.0f;
            });
        }
        ++it;
    }

    // 2b. Paint temporary vision areas (scrying, Eye of Rhianne)
    // Single-pass: paint, decrement, and compact in place
    {
        size_t write = 0;
        for (size_t read = 0; read < temp_visions_.size(); ++read) {
            auto& tv = temp_visions_[read];
            if (tv.remaining_ticks > 0) {
                visibility_grid_->paint_circle(tv.army, tv.x, tv.z, tv.radius,
                                               map::VisFlag::Vision);
                tv.remaining_ticks--;
                if (tv.remaining_ticks > 0) {
                    if (write != read) temp_visions_[write] = tv;
                    write++;
                }
            }
        }
        temp_visions_.resize(write);
    }

    // 3. Share allied vision
    u32 n = static_cast<u32>(
        std::min(army_count(),
                 static_cast<size_t>(map::VisibilityGrid::MAX_ARMIES)));
    for (u32 a = 0; a < n; ++a) {
        for (u32 b = a + 1; b < n; ++b) {
            if (is_ally(static_cast<i32>(a), static_cast<i32>(b))) {
                visibility_grid_->merge_armies(a, b);
                visibility_grid_->merge_armies(b, a);
            }
        }
    }

    // 3.5. Update blip cache (dead-reckoning positions)
    entity_registry_.for_each_unit([&](Entity& e) {
        if (e.destroyed() || !e.is_unit()) return;
        u32 eid = e.entity_id();
        // Cache per-unit stealth state before the army loop to avoid
        // redundant is_intel_enabled string lookups per army iteration.
        auto* unit = static_cast<Unit*>(&e);
        bool radar_stealth = unit->has_radar_stealth();
        bool sonar_stealth = unit->has_sonar_stealth();
        bool cloaked = unit->is_cloaked();
        for (u32 a = 0; a < n; ++a) {
            if (static_cast<i32>(a) == e.army()) continue; // skip own army
            if (has_any_intel_cached(&e, a, radar_stealth, sonar_stealth,
                                     cloaked)) {
                // Army can see entity — update cached snapshot
                auto& snap = blip_cache_[eid][a];
                snap.last_known_position = e.position();
                snap.blueprint_id = e.blueprint_id();
                snap.entity_army = e.army();
                snap.entity_dead = false;
            }
            // If no intel, keep stale data — that IS the dead-reckoning freeze
        }
    });

    // Erase destroyed entities from blip cache (prevents unbounded growth)
    for (auto it = los_ever_.begin(); it != los_ever_.end();) {
        const auto* e = entity_registry_.find(it->first);
        it = !e || e->destroyed() ? los_ever_.erase(it) : std::next(it);
    }
    for (auto it = blip_cache_.begin(); it != blip_cache_.end(); ) {
        auto* e = entity_registry_.find(it->first);
        if (!e || e->destroyed()) {
            it = blip_cache_.erase(it);
        } else {
            ++it;
        }
    }

    // 4. Detect changes and fire OnIntelChange (stealth-aware)
    std::vector<u32> ids;
    ids.reserve(entity_registry_.count());
    entity_registry_.for_each_unit([&](Entity& e) {
        if (!e.destroyed() && e.is_unit())
            ids.push_back(e.entity_id());
    });

    for (u32 eid : ids) {
        auto* e = entity_registry_.find(eid);
        if (!e || e->destroyed() || !e->is_unit()) continue;

        auto& pos = e->position();

        // Cache per-unit stealth state before the army loop.
        auto* u = static_cast<const Unit*>(e);
        bool radar_stealth = u->has_radar_stealth();
        bool sonar_stealth = u->has_sonar_stealth();
        bool cloaked = u->is_cloaked();

        for (u32 a = 0; a < n; ++a) {
            if (static_cast<i32>(a) == e->army()) continue; // skip own army

            bool cur_omn =
                visibility_grid_->has_omni(pos.x, pos.z, a);
            bool cur_vis = visibility_grid_->has_vision(pos.x, pos.z, a) &&
                           (!cloaked || cur_omn);
            bool cur_rad = has_effective_radar_cached(e, a, radar_stealth);
            bool cur_son = has_effective_sonar_cached(e, a, sonar_stealth);

            auto prev_it = prev_entity_vis_.find(eid);
            EntityVisSnapshot prev;
            if (prev_it != prev_entity_vis_.end())
                prev = prev_it->second[a];

            if (prev.vision != cur_vis) {
                fire_on_intel_change(eid, a, "LOSNow", cur_vis);
                e = entity_registry_.find(eid);
                if (!e || e->destroyed()) break;
            }
            if (prev.radar != cur_rad) {
                fire_on_intel_change(eid, a, "Radar", cur_rad);
                e = entity_registry_.find(eid);
                if (!e || e->destroyed()) break;
            }
            if (prev.sonar != cur_son) {
                fire_on_intel_change(eid, a, "Sonar", cur_son);
                e = entity_registry_.find(eid);
                if (!e || e->destroyed()) break;
            }
            if (prev.omni != cur_omn) {
                fire_on_intel_change(eid, a, "Omni", cur_omn);
                e = entity_registry_.find(eid);
                if (!e || e->destroyed()) break;
            }
        }
    }

    // 5. Save current state for next tick (stealth-aware)
    prev_entity_vis_.clear();
    entity_registry_.for_each_unit([&](Entity& e) {
        if (e.destroyed() || !e.is_unit()) return;
        auto& pos = e.position();
        auto* unit = static_cast<const Unit*>(&e);
        bool radar_stealth = unit->has_radar_stealth();
        bool sonar_stealth = unit->has_sonar_stealth();
        bool cloaked = unit->is_cloaked();
        std::array<EntityVisSnapshot, MAX_VIS_ARMIES> states{};
        for (u32 a = 0; a < n; ++a) {
            states[a].omni =
                visibility_grid_->has_omni(pos.x, pos.z, a);
            states[a].vision = visibility_grid_->has_vision(pos.x, pos.z, a) &&
                               (!cloaked || states[a].omni);
            states[a].radar = has_effective_radar_cached(&e, a, radar_stealth);
            states[a].sonar = has_effective_sonar_cached(&e, a, sonar_stealth);
            if (states[a].vision) los_ever_[e.entity_id()] |= 1u << a;
        }
        prev_entity_vis_[e.entity_id()] = states;
    });
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
    lua_rawget(L_, brain_tbl);
    if (!lua_isfunction(L_, -1)) {
        lua_pop(L_, 2); // pop non-function + brain_tbl
        return;
    }

    lua_pushvalue(L_, brain_tbl); // self (brain)

    // Build blip table: {_c_entity_id, _c_req_army}. No pointer to the unit,
    // as unit:GetBlip's blips: AI scripts keep the blips they're told of past
    // their unit, and its memory with it; every use resolves the id.
    lua_newtable(L_);
    int blip_tbl = lua_gettop(L_);
    lua_pushstring(L_, "_c_entity_id");
    lua_pushnumber(L_, entity->entity_id());
    lua_rawset(L_, blip_tbl);
    lua_pushstring(L_, "_c_req_army");
    lua_pushnumber(L_, static_cast<lua_Number>(army_idx));
    lua_rawset(L_, blip_tbl);

    // Set __osc_blip_mt metatable (lazy-build, same pattern as unit_GetBlip)
    lua_pushstring(L_, "__osc_blip_mt");
    lua_rawget(L_, LUA_REGISTRYINDEX);
    if (lua_istable(L_, -1)) {
        lua_setmetatable(L_, blip_tbl);
    } else {
        lua_pop(L_, 1); // no metatable cached yet — skip
    }

    lua_pushstring(L_, recon_type);
    lua_pushboolean(L_, val ? 1 : 0);

    if (lua_pcall(L_, 4, 0, 0) != 0) {
        spdlog::warn("OnIntelChange error: {}", lua_tostring(L_, -1));
        lua_pop(L_, 1);
    }

    lua_pop(L_, 1); // pop brain_tbl
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

void SimState::kill_unit(Unit& unit) {
    if (unit.destroyed() || unit.is_dying()) return;
    const u32 id = unit.entity_id();
    if (L_ && unit.lua_table_ref() >= 0) {
        const int top = lua_gettop(L_);
        lua_rawgeti(L_, LUA_REGISTRYINDEX, unit.lua_table_ref());
        lua_pushstring(L_, "Kill");
        lua_gettable(L_, -2);
        if (lua_isfunction(L_, -1)) {
            lua_pushvalue(L_, top + 1);
            if (lua_pcall(L_, 1, 0, 0) != 0) {
                const char* err = lua_tostring(L_, -1);
                spdlog::warn("Kill error: {}", err ? err : "(unknown)");
            }
            lua_settop(L_, top);
            return;
        }
        lua_settop(L_, top);
    }
    unit.mark_destroyed();
    entity_registry_.unregister_entity(id);
}

void SimState::dispose_defeated_army(i32 army) {
    const i32 recipient = find_share_recipient(army);
    // PartialShare transfers only structures + engineers; the rest are destroyed.
    const bool partial = share_mode_ == ShareMode::PartialShare;
    bool transferred_any = false;
    std::vector<u32> to_kill;
    entity_registry_.for_each_unit([&](Entity& e) {
        if (e.army() != army || e.destroyed() || !e.is_unit()) return;
        auto* u = static_cast<Unit*>(&e);
        bool transfer = recipient >= 0;
        if (transfer && partial) {
            transfer = u->has_category("STRUCTURE") || u->has_category("ENGINEER");
        }
        if (transfer) {
            u->set_army(recipient);
            // Keep the Lua-side Army field (1-based) in sync, mirroring capture.
            if (u->lua_table_ref() >= 0) {
                lua_rawgeti(L_, LUA_REGISTRYINDEX, u->lua_table_ref());
                lua_pushstring(L_, "Army");
                lua_pushnumber(L_, recipient + 1);
                lua_rawset(L_, -3);
                lua_pop(L_, 1);
            }
            transferred_any = true;
        } else if (!u->is_dying()) {
            to_kill.push_back(u->entity_id());
        }
    });
    // Killed after the walk: a death runs scripts, which may kill others.
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

namespace {

/// FNV-1a over 64-bit words.
struct Fnv {
    u64 h = 1469598103934665603ULL;
    void mix(u64 v) {
        h ^= v;
        h *= 1099511628211ULL;
    }
    void mix_f32(f32 f) {
        u32 bits;
        std::memcpy(&bits, &f, sizeof(bits));
        mix(bits);
    }
};

} // namespace

std::array<u64, SimState::ChecksumParts::kCount> SimState::ChecksumParts::values() const {
    return {rng,     armies,      entities, units,          orders, navigation,
            weapons, projectiles, shields,  economy_events, threads};
}

u32 SimState::ChecksumParts::total() const {
    Fnv f;
    for (const u64 part : values()) f.mix(part);
    return static_cast<u32>(f.h ^ (f.h >> 32));
}

SimState::ChecksumParts SimState::checksum_parts() const {
    ChecksumParts parts;
    parts.rng = sim_random_.state();
    const auto mix_vec = [](Fnv& f, const Vector3& v) {
        f.mix_f32(v.x);
        f.mix_f32(v.y);
        f.mix_f32(v.z);
    };
    const auto mix_str = [](Fnv& f, const std::string& str) {
        f.mix(static_cast<u64>(str.size()));
        for (const char ch : str) f.mix(static_cast<u8>(ch));
    };

    Fnv armies;
    armies.mix(tick_count_);
    for (const auto& a : armies_) {
        armies.mix(static_cast<u64>(a->state()));
        const auto& econ = a->economy();
        for (const auto* res : {&econ.mass, &econ.energy}) {
            armies.mix_f32(static_cast<f32>(res->stored));
            armies.mix_f32(static_cast<f32>(res->max_storage));
            armies.mix_f32(static_cast<f32>(res->income));
            armies.mix_f32(static_cast<f32>(res->requested));
        }
        // Its influence map's entries (M207b), as Moho mixes their strengths.
        if (const InfluenceMap* map = a->influence_map()) {
            armies.mix(static_cast<u64>(map->entry_count()));
            map->for_each_entry([&](const InfluenceMap::EntryView& v) {
                armies.mix(v.id);
                armies.mix_f32(v.strength);
            });
        }
    }
    parts.armies = armies.h;

    // The registry walks in id order.
    Fnv entities, units, orders, navigation, weapons, projectiles, shields;
    entity_registry_.for_each([&](const Entity& e) {
        entities.mix(e.entity_id());
        entities.mix(static_cast<u64>(static_cast<u32>(e.army())));
        entities.mix(e.destroyed() ? 1u : 0u);
        mix_vec(entities, e.position());
        const auto& q = e.orientation();
        entities.mix_f32(q.x);
        entities.mix_f32(q.y);
        entities.mix_f32(q.z);
        entities.mix_f32(q.w);
        entities.mix_f32(e.health());
        entities.mix_f32(e.fraction_complete());

        if (e.is_projectile()) {
            const auto& p = static_cast<const Projectile&>(e);
            projectiles.mix(e.entity_id());
            mix_vec(projectiles, p.velocity);
            projectiles.mix(p.target_entity_id);
            mix_vec(projectiles, p.target_position);
            projectiles.mix_f32(p.lifetime);
            projectiles.mix((p.impacted ? 1u : 0u) | (p.tracking ? 2u : 0u));
        }
        if (e.is_shield()) {
            shields.mix(e.entity_id());
            shields.mix(static_cast<const Shield&>(e).is_on ? 1u : 0u);
        }
        if (!e.is_unit()) return;
        const auto& u = static_cast<const Unit&>(e);

        // The player's settings: a pause or fire state that differs between
        // peers is a desync before it moves anything.
        units.mix(e.entity_id());
        units.mix(u.script_bits());
        units.mix(static_cast<u64>(static_cast<u32>(u.fire_state())));
        units.mix((u.is_paused() ? 1u : 0u) | (u.auto_mode() ? 2u : 0u) |
                  (u.repeat_queue() ? 4u : 0u) | (u.auto_surface_mode() ? 8u : 0u) |
                  (u.is_dying() ? 16u : 0u) | (u.is_being_built() ? 32u : 0u) |
                  (u.factory_assist_build() ? 64u : 0u));
        // An assist build's roll-off, only while under way.
        if (u.assist_rolloff_wait() != 0)
            units.mix(0x524f4c4c00000000ull | static_cast<u32>(u.assist_rolloff_wait())); // "ROLL"
        // What a carrier keeps inside (M206q), only when it keeps something.
        if (!u.stored_ids().empty()) {
            units.mix(0x53544f5200000000ull | u.stored_ids().size()); // "STOR"
            for (u32 id : u.stored_ids()) units.mix(id);
        }
        // A stun, only while it lasts.
        if (u.stun_ticks() > 0)
            units.mix(0x5354554e00000000ull | static_cast<u32>(u.stun_ticks())); // "STUN"
        mix_str(units, u.layer());
        // A sub's depth and dive (M206o), only when under or on its way, so
        // other units hash as before.
        if (u.sub_elevation() != 0.0f || u.diving() || u.surfacing()) {
            units.mix_f32(u.sub_elevation());
            units.mix((u.diving() ? 1u : 0u) | (u.surfacing() ? 2u : 0u));
        }
        units.mix(u.transport_id());
        units.mix(static_cast<u64>(u.cargo_ids().size()));
        // Who holds which of a transport's attach points (M206l).
        if (const auto* slots = u.built_transport_slots()) {
            for (const auto& slot : slots->slots()) {
                units.mix(slot.unit_id);
                units.mix(static_cast<u64>(static_cast<u32>(slot.bone)));
            }
        }
        units.mix(u.build_target_id());
        units.mix(u.reclaim_target_id());
        units.mix(u.repair_target_id());
        units.mix(u.capture_target_id());
        units.mix_f32(u.work_progress());
        units.mix(static_cast<u64>(static_cast<u32>(u.nuke_silo_ammo())));
        units.mix(static_cast<u64>(static_cast<u32>(u.tactical_silo_ammo())));
        units.mix(static_cast<u64>(static_cast<u32>(u.silo_build().weapon)));
        units.mix_f32(static_cast<f32>(u.silo_build().progress));
        const auto& econ = u.economy();
        units.mix_f32(static_cast<f32>(econ.consumption_mass));
        units.mix_f32(static_cast<f32>(econ.consumption_energy));
        units.mix_f32(static_cast<f32>(econ.production_mass));
        units.mix_f32(static_cast<f32>(econ.production_energy));
        // Docked at a staging platform (M206r), only then: its tank and its
        // repair's ask.
        if (u.refuel_started() || econ.dock_repair_energy != 0 || econ.dock_repair_mass != 0) {
            units.mix(0x4655454c00000000ull); // "FUEL"
            units.mix_f32(u.fuel_ratio());
            units.mix_f32(static_cast<f32>(econ.dock_repair_energy));
            units.mix_f32(static_cast<f32>(econ.dock_repair_mass));
        }

        orders.mix(e.entity_id());
        orders.mix(static_cast<u64>(u.command_queue().size()));
        for (const UnitCommand& cmd : u.command_queue()) {
            orders.mix(static_cast<u64>(cmd.type));
            orders.mix(cmd.target_id);
            mix_vec(orders, cmd.target_pos);
            orders.mix(cmd.command_id);
            mix_str(orders, cmd.blueprint_id);
            orders.mix((cmd.launched ? 1u : 0u) | (cmd.started ? 2u : 0u) |
                       (cmd.approached ? 4u : 0u) | (cmd.in_band ? 8u : 0u));
            orders.mix(cmd.beacon_id);
            orders.mix(cmd.assigned_id);
            // A factory rolling its unit off, only then (as the cargo below).
            if (cmd.rolloff_wait != 0) {
                orders.mix(0x524f4c4cu); // "ROLL"
                orders.mix(static_cast<u64>(static_cast<u32>(cmd.rolloff_wait)));
            }
            // A refuel under way (M206r), only once it has a slot or waits.
            if (cmd.dock_phase != DockPhase::Reserve || cmd.dock_wait != 0) {
                orders.mix(0x444f434bu); // "DOCK"
                orders.mix(static_cast<u64>(cmd.dock_phase));
                orders.mix(static_cast<u64>(static_cast<u32>(cmd.dock_wait)));
            }
            // A carrier's launch under way (M206q), only then.
            if (cmd.launch_wait >= 0) {
                orders.mix(0x4c4e4348u); // "LNCH"
                orders.mix(static_cast<u64>(static_cast<u32>(cmd.launch_wait)));
                orders.mix(static_cast<u64>(cmd.launch_queue.size()));
                for (u32 id : cmd.launch_queue) orders.mix(id);
            }
            // A specific unload's cargo; only when set, so other orders hash
            // as before it existed.
            if (!cmd.unload_ids.empty()) {
                orders.mix(static_cast<u64>(cmd.unload_ids.size()));
                for (u32 id : cmd.unload_ids) orders.mix(id);
            }
        }
        // A transport's pickup and a unit's beam up (M206m), only while
        // under way.
        if (u.pickup_running() || u.beam_up_ticks() > 0) {
            orders.mix(0x5049434bu); // "PICK"
            orders.mix((u.pickup_running() ? 1u : 0u) | (u.pickup_ready() ? 2u : 0u));
            orders.mix(static_cast<u64>(static_cast<u32>(u.pickup_ticks())));
            orders.mix(static_cast<u64>(u.pickup_ids().size()));
            for (const u32 id : u.pickup_ids()) orders.mix(id);
            orders.mix(static_cast<u64>(static_cast<u32>(u.beam_up_ticks())));
        }
        // A factory's rally orders (M206j), only where it has some.
        if (!u.rally_orders().empty()) {
            orders.mix(0x52414c4cu); // "RALL": apart from the queue above
            orders.mix(static_cast<u64>(u.rally_orders().size()));
            for (const UnitCommand& cmd : u.rally_orders()) {
                orders.mix(static_cast<u64>(cmd.type));
                orders.mix(cmd.target_id);
                mix_vec(orders, cmd.target_pos);
                orders.mix(cmd.command_id);
            }
        }

        navigation.mix(e.entity_id());
        navigation.mix(static_cast<u64>(u.navigator().status()));
        mix_vec(navigation, u.navigator().goal());
        mix_vec(navigation, u.velocity());

        weapons.mix(e.entity_id());
        for (const auto& w : u.weapons()) {
            weapons.mix(w->target_entity_id);
            weapons.mix((w->has_ground_target ? 1u : 0u) | (w->enabled ? 2u : 0u));
            if (w->has_ground_target) mix_vec(weapons, w->ground_target);
            weapons.mix(w->fire_clock);
        }
    });
    parts.entities = entities.h;
    parts.units = units.h;
    parts.orders = orders.h;
    parts.navigation = navigation.h;
    parts.weapons = weapons.h;
    parts.projectiles = projectiles.h;
    parts.shields = shields.h;

    Fnv events;
    economy_events_.for_each([&](const EconomyEvent& evt) {
        events.mix(evt.unit_id());
        events.mix_f32(static_cast<f32>(evt.progress()));
        events.mix((evt.is_done() ? 1u : 0u) | (evt.is_cancelled() ? 2u : 0u));
    });
    parts.economy_events = events.h;

    Fnv threads;
    thread_manager_.for_each_live([&](u64 serial, i32 wake) {
        threads.mix(serial);
        threads.mix(static_cast<u64>(static_cast<u32>(wake)));
    });
    parts.threads = threads.h;
    return parts;
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
    std::string out = fmt::format("T {} rng {:016x}\n", tick_count_, sim_random_.state());
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

const SimState::ChecksumParts& SimState::tick_checksum() const {
    if (!tick_checksum_valid_ || tick_checksum_tick_ != tick_count_) {
        tick_checksum_ = checksum_parts();
        tick_checksum_tick_ = tick_count_;
        tick_checksum_valid_ = true;
    }
    return tick_checksum_;
}

u32 SimState::compute_sync_checksum() const {
    return checksum_parts().total();
}

i32 SimState::player_result() const {
    const ArmyBrain* player = army_at(0);
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
        if (!b || b->is_civilian() || static_cast<i32>(i) == 0) continue;
        if (player->is_ally(static_cast<i32>(i))) continue;
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
