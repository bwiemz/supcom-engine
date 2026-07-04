#include "sim/sim_state.hpp"
#include "sim/anim_cache.hpp"
#include "sim/bone_cache.hpp"
#include "audio/sound_manager.hpp"
#include "core/profiler.hpp"
#include "map/pathfinder.hpp"
#include "map/pathfinding_grid.hpp"
#include "map/terrain.hpp"
#include "map/visibility_grid.hpp"
#include "sim/entity.hpp"
#include "sim/projectile.hpp"
#include "sim/unit.hpp"

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

#include <spdlog/spdlog.h>

#include <algorithm>
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
}

SimState::~SimState() {
    // Clear sound manager lightuserdata from Lua registry before the
    // unique_ptr is destroyed, preventing a dangling pointer if any
    // Lua __gc metamethods fire during VM shutdown.
    if (L_ && sound_manager_) {
        lua_pushstring(L_, "osc_sound_manager");
        lua_pushnil(L_);
        lua_rawset(L_, LUA_REGISTRYINDEX);
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

void SimState::set_fog_of_war(std::string mode) {
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

void SimState::set_sound_manager(std::unique_ptr<audio::SoundManager> mgr) {
    sound_manager_ = std::move(mgr);
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
    if (recording_) {
        recorded_replay_.commands.push_back(sc);
        if (exec_tick > recorded_replay_.final_tick)
            recorded_replay_.final_tick = exec_tick;
        recorded_replay_.command_delay = command_delay_;
        recorded_replay_.victory_condition = victory_condition_;
    }
    command_scheduler_.submit(std::move(sc));
    return exec_tick;
}

void SimState::route_command(const std::vector<u32>& unit_ids,
                             const UnitCommand& command, bool clear_existing) {
    // Local human order under an active network session → broadcast + schedule
    // so every peer runs it on the same tick.
    if (local_command_sink_ && human_input_active_) {
        local_command_sink_(unit_ids, command, clear_existing);
        return;
    }
    // Single-player, or a deterministic AI/sim order in multiplayer: apply now.
    // (AI runs identically on every client, so its orders stay in sync without
    // being sent over the wire.)
    for (u32 uid : unit_ids) {
        auto* e = entity_registry_.find(uid);
        if (!e || e->destroyed() || !e->is_unit()) continue;
        auto* unit = static_cast<Unit*>(e);
        // Stop clears the queue outright (rather than queueing a Stop order), so
        // it matches the old IssueStop's immediate clear_commands() semantics.
        if (command.type == CommandType::Stop) unit->clear_commands();
        else unit->push_command(command, clear_existing);
    }
}

void SimState::queue_replay(const Replay& replay) {
    command_delay_ = replay.command_delay;
    if (!replay.victory_condition.empty())
        set_victory_condition(replay.victory_condition);
    for (const auto& c : replay.commands) command_scheduler_.submit(c);
}

void SimState::dispatch_due_commands() {
    PROFILE_ZONE("Sim::commands");
    const bool no_rush = no_rush_active();
    command_scheduler_.dispatch_due(tick_count_, [&](const ScheduledCommand& sc) {
        for (u32 uid : sc.unit_ids) {
            auto* e = entity_registry_.find(uid);
            if (!e || e->destroyed() || !e->is_unit()) continue;
            auto* unit = static_cast<Unit*>(e);
            // A scheduled Stop clears the queue (mirrors route_command's direct
            // branch), so a networked player's Stop lands identically on peers.
            if (sc.command.type == CommandType::Stop) {
                unit->clear_commands();
                continue;
            }
            UnitCommand cmd = sc.command;
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
    entity_registry_.for_each([&](Entity& e) {
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

    // Apply the commands scheduled for this tick before anything simulates,
    // so orders take effect deterministically at the start of the frame.
    dispatch_due_commands();

    if (pathfinder_) {
        pathfinder_->reset_request_count();
    }

    {
        PROFILE_ZONE("Sim::threads");
        thread_manager_.resume_all(tick_count_);
    }

    update_economies();
    update_entities();

    // Process air crash impacts
    {
        std::vector<u32> crash_impacts;
        entity_registry_.for_each([&](Entity& e) {
            if (e.destroyed() || !e.is_unit()) return;
            auto* unit = static_cast<Unit*>(&e);
            if (unit->crash_impacted()) {
                crash_impacts.push_back(e.entity_id());
            }
        });

        for (u32 crash_id : crash_impacts) {
            auto* ce = entity_registry_.find(crash_id);
            if (!ce || ce->destroyed()) continue;
            auto* crash_unit = static_cast<Unit*>(ce);

            f32 crash_radius = crash_unit->footprint_size_x() * 1.5f;
            if (crash_radius < 2.0f) crash_radius = 2.0f;
            f32 dmg = crash_unit->crash_damage();
            auto nearby = entity_registry_.collect_in_radius(
                ce->position().x, ce->position().z, crash_radius);
            for (u32 nid : nearby) {
                if (nid == crash_id) continue;
                auto* ne = entity_registry_.find(nid);
                if (!ne || ne->destroyed()) continue;
                f32 new_hp = ne->health() - dmg;
                ne->set_health(new_hp);
                if (new_hp <= 0 && ne->is_unit()) {
                    static_cast<Unit*>(ne)->begin_dying(0.1f);
                }
            }

            add_death_event(ce->position().x, ce->position().y,
                            ce->position().z, crash_radius, ce->army());
            ce->mark_destroyed();
        }
    }

    // "No Rush": pin units that strayed beyond their confinement radius.
    enforce_no_rush();

    update_visibility();

    // --- Victory-condition enforcement (mode + team aware) ---
    update_victory();

    // Audio: clean up finished one-shot sounds
    if (sound_manager_) {
        PROFILE_ZONE("Sim::audio_gc");
        sound_manager_->gc();
    }

    // Economy events: tick drains, wake waiting threads on completion
    {
        PROFILE_ZONE("Sim::econ_events");
        tick_economy_events();
    }

    // VFX: expire timed effects (decals, splats) and garbage collect destroyed ones
    {
        PROFILE_ZONE("Sim::vfx_gc");
        effect_registry_.expire_timed(game_time_);
        effect_registry_.gc();
    }

    // Periodic Lua garbage collection to prevent unbounded memory growth.
    // Lua 5.0 uses stop-the-world mark-and-sweep GC. Setting threshold to 0
    // forces an immediate full collection. Running every 50 ticks (5 seconds
    // game time) amortizes GC cost while preventing heap growth.
    if (tick_count_ % 50 == 0) {
        PROFILE_ZONE("Sim::lua_gc");
        lua_setgcthreshold(L_, 0);
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

void SimState::tick_economy_events() {
    economy_events_.tick(SECONDS_PER_TICK);
    // Wake threads waiting on completed/cancelled events
    economy_events_.for_each([&](EconomyEvent& evt) {
        if ((evt.is_done() || evt.is_cancelled()) && evt.waiting_thread_ref() >= 0) {
            thread_manager_.wake_thread(evt.waiting_thread_ref(), tick_count_);
            evt.set_waiting_thread_ref(-2);
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
            static_cast<Unit*>(e)->update(SECONDS_PER_TICK, ctx);
        } else if (e->is_projectile()) {
            static_cast<Projectile*>(e)->update(SECONDS_PER_TICK,
                                                 entity_registry_, L_, terrain_.get());
        }
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

    // 2. Paint intel radii for each unit
    entity_registry_.for_each([&](Entity& e) {
        if (e.destroyed() || !e.is_unit()) return;
        auto* unit = static_cast<Unit*>(&e);
        i32 army = unit->army();
        if (army < 0 ||
            army >= static_cast<i32>(map::VisibilityGrid::MAX_ARMIES))
            return;

        auto& pos = unit->position();
        u32 ua = static_cast<u32>(army);

        // Vision: terrain LOS occlusion
        if (unit->is_intel_enabled("Vision")) {
            f32 r = unit->get_intel_radius("Vision");
            if (r > 0.0f) {
                f32 eye_h = terrain_->get_terrain_height(pos.x, pos.z) +
                            map::VisibilityGrid::EYE_OFFSET;
                visibility_grid_->paint_circle_los(ua, pos.x, pos.z, r,
                                                    eye_h);
            }
        }

        // WaterVision maps to Vision flag but no terrain LOS (underwater sensing)
        if (unit->is_intel_enabled("WaterVision")) {
            f32 r = unit->get_intel_radius("WaterVision");
            if (r > 0.0f)
                visibility_grid_->paint_circle(ua, pos.x, pos.z, r,
                                               map::VisFlag::Vision);
        }

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
        for (auto& m : non_los) {
            if (unit->is_intel_enabled(m.type)) {
                f32 r = unit->get_intel_radius(m.type);
                if (r > 0.0f)
                    visibility_grid_->paint_circle(ua, pos.x, pos.z, r,
                                                   m.flag);
            }
        }

        // Self-vision: own army always sees own unit cell
        visibility_grid_->paint_circle(
            ua, pos.x, pos.z,
            static_cast<f32>(map::VisibilityGrid::CELL_SIZE) * 0.5f,
            map::VisFlag::Vision);
    });

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
    entity_registry_.for_each([&](Entity& e) {
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
    entity_registry_.for_each([&](Entity& e) {
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
    entity_registry_.for_each([&](Entity& e) {
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

    // Build blip table: {_c_object, _c_entity_id, _c_req_army}
    lua_newtable(L_);
    int blip_tbl = lua_gettop(L_);
    lua_pushstring(L_, "_c_object");
    lua_pushlightuserdata(L_, entity);
    lua_rawset(L_, blip_tbl);
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
constexpr f32 kDefeatDeathDuration = 2.0f; // death animation for destroyed units
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

void SimState::dispose_defeated_army(i32 army) {
    const i32 recipient = find_share_recipient(army);
    // PartialShare transfers only structures + engineers; the rest are destroyed.
    const bool partial = share_mode_ == ShareMode::PartialShare;
    bool transferred_any = false;
    entity_registry_.for_each([&](Entity& e) {
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
            u->begin_dying(kDefeatDeathDuration);
        }
    });
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
    if (game_ended_ || victory_mode_ == VictoryMode::Sandbox) return;

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
    entity_registry_.for_each([&](Entity& e) {
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

u32 SimState::compute_sync_checksum() const {
    // FNV-1a hash over the authoritative sim state.
    u64 h = 1469598103934665603ULL;
    auto mix = [&](u64 v) {
        h ^= v;
        h *= 1099511628211ULL;
    };
    auto mix_f32 = [&](f32 f) {
        u32 bits;
        std::memcpy(&bits, &f, sizeof(bits));
        mix(bits);
    };

    mix(tick_count_);

    // Army state and stored economy.
    for (const auto& a : armies_) {
        mix(static_cast<u64>(a->state()));
        mix_f32(static_cast<f32>(a->economy().mass.stored));
        mix_f32(static_cast<f32>(a->economy().energy.stored));
    }

    // Entities in a deterministic order (the registry hash-map order is not
    // stable, so sort by id first).
    std::vector<u32> ids;
    ids.reserve(entity_registry_.count());
    entity_registry_.for_each([&](const Entity& e) { ids.push_back(e.entity_id()); });
    std::sort(ids.begin(), ids.end());
    for (u32 id : ids) {
        const Entity* e = entity_registry_.find(id);
        if (!e) continue;
        mix(id);
        mix(static_cast<u64>(static_cast<u32>(e->army())));
        mix(e->destroyed() ? 1u : 0u);
        const auto& p = e->position();
        mix_f32(p.x);
        mix_f32(p.y);
        mix_f32(p.z);
        mix_f32(e->health());
    }

    return static_cast<u32>(h ^ (h >> 32));
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
