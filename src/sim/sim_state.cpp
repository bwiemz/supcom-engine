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
    return visibility_grid_->has_vision(pos.x, pos.z, req_army) ||
           has_effective_radar(entity, req_army) ||
           has_effective_sonar(entity, req_army) ||
           visibility_grid_->has_omni(pos.x, pos.z, req_army);
}

void SimState::tick() {
    PROFILE_ZONE("Sim::tick");
    tick_count_++;
    game_time_ = tick_count_ * SECONDS_PER_TICK;

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

    update_visibility();

    // Defeat detection: mark armies with no living units as defeated
    // (simplified demoralization — FA's CheckVictory Lua thread handles real logic)
    if (!game_ended_ && tick_count_ > 50) { // grace period: skip first 5 seconds
        for (auto& army : armies_) {
            if (army->is_defeated() || army->is_civilian()) continue;
            if (army->get_unit_cost_total(entity_registry_) == 0) {
                army->set_state(BrainState::Defeat);
                spdlog::info("Army {} ({}) defeated — no units remaining",
                             army->index(), army->name());
            }
        }
    }

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
}

void SimState::update_economies() {
    PROFILE_ZONE("Sim::economy");
    for (auto& army : armies_) {
        army->update_economy(entity_registry_, SECONDS_PER_TICK);
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
                   visibility_grid_.get(), {}};
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
        for (u32 a = 0; a < n; ++a) {
            if (static_cast<i32>(a) == e.army()) continue; // skip own army
            if (has_any_intel(&e, a)) {
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
    {
        std::vector<u32> dead_ids;
        for (auto& [eid, _] : blip_cache_) {
            auto* e = entity_registry_.find(eid);
            if (!e || e->destroyed())
                dead_ids.push_back(eid);
        }
        for (u32 eid : dead_ids)
            blip_cache_.erase(eid);
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

        for (u32 a = 0; a < n; ++a) {
            if (static_cast<i32>(a) == e->army()) continue; // skip own army

            bool cur_vis =
                visibility_grid_->has_vision(pos.x, pos.z, a);
            bool cur_rad = has_effective_radar(e, a);
            bool cur_son = has_effective_sonar(e, a);
            bool cur_omn =
                visibility_grid_->has_omni(pos.x, pos.z, a);

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
        std::array<EntityVisSnapshot, MAX_VIS_ARMIES> states{};
        for (u32 a = 0; a < n; ++a) {
            states[a].vision =
                visibility_grid_->has_vision(pos.x, pos.z, a);
            states[a].radar = has_effective_radar(&e, a);
            states[a].sonar = has_effective_sonar(&e, a);
            states[a].omni =
                visibility_grid_->has_omni(pos.x, pos.z, a);
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

i32 SimState::player_result() const {
    if (!game_ended_) {
        // Check if player army (index 0) is defeated
        auto* player = army_at(0);
        if (player && player->is_defeated()) return 2; // defeat

        // Check if all non-civilian enemy armies are defeated
        bool all_enemies_dead = true;
        for (size_t i = 0; i < army_count(); i++) {
            auto* brain = army_at(i);
            if (!brain || brain->is_civilian()) continue;
            if (static_cast<i32>(i) == 0) continue; // skip player
            if (player && player->is_ally(static_cast<i32>(i))) continue;
            if (!brain->is_defeated()) {
                all_enemies_dead = false;
                break;
            }
        }
        if (all_enemies_dead && army_count() > 1) return 1; // victory

        return 0; // in progress
    }

    // Game ended — determine result from brain states
    auto* player = army_at(0);
    if (!player) return 3; // draw
    if (player->state() == BrainState::Victory) return 1;
    if (player->state() == BrainState::Defeat ||
        player->state() == BrainState::Recalled) return 2;
    return 3; // draw
}

} // namespace osc::sim
