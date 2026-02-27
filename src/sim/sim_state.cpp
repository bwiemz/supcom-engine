#include "sim/sim_state.hpp"
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

SimState::SimState(lua_State* L, blueprints::BlueprintStore* store)
    : L_(L), thread_manager_(L), blueprint_store_(store) {}

SimState::~SimState() = default;

void SimState::set_terrain(std::unique_ptr<map::Terrain> terrain) {
    terrain_ = std::move(terrain);
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
    spdlog::info("Built visibility grid: {}x{} cells (cell_size={})",
                 visibility_grid_->grid_width(),
                 visibility_grid_->grid_height(),
                 visibility_grid_->cell_size());
}

void SimState::tick() {
    tick_count_++;
    game_time_ = tick_count_ * SECONDS_PER_TICK;
    thread_manager_.resume_all(tick_count_);
    update_economies();
    update_entities();
    update_visibility();
}

void SimState::update_economies() {
    for (auto& army : armies_) {
        army->update_economy(entity_registry_, SECONDS_PER_TICK);
    }
}

void SimState::update_entities() {
    // Snapshot IDs to avoid iterator invalidation if update() triggers removal
    std::vector<u32> ids;
    ids.reserve(entity_registry_.count());
    entity_registry_.for_each([&](Entity& e) {
        ids.push_back(e.entity_id());
    });

    SimContext ctx{entity_registry_, L_, terrain_.get(),
                   pathfinder_.get(), pathfinding_grid_.get(),
                   visibility_grid_.get()};

    for (u32 id : ids) {
        auto* e = entity_registry_.find(id);
        if (!e || e->destroyed()) continue;
        if (e->is_unit()) {
            static_cast<Unit*>(e)->update(SECONDS_PER_TICK, ctx);
        } else if (e->is_projectile()) {
            static_cast<Projectile*>(e)->update(SECONDS_PER_TICK,
                                                 entity_registry_, L_);
        }
    }
}

void SimState::update_visibility() {
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

        struct IntelMapping {
            const char* type;
            map::VisFlag flag;
        };
        static const IntelMapping mappings[] = {
            {"Vision", map::VisFlag::Vision},
            {"WaterVision", map::VisFlag::Vision},
            {"Radar", map::VisFlag::Radar},
            {"Sonar", map::VisFlag::Sonar},
            {"Omni", map::VisFlag::Omni},
        };

        for (auto& m : mappings) {
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

    // 4. Detect changes and fire OnIntelChange
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
            bool cur_rad =
                visibility_grid_->has_radar(pos.x, pos.z, a);
            bool cur_son =
                visibility_grid_->has_sonar(pos.x, pos.z, a);
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

    // 5. Save current state for next tick
    prev_entity_vis_.clear();
    entity_registry_.for_each([&](Entity& e) {
        if (e.destroyed() || !e.is_unit()) return;
        auto& pos = e.position();
        std::array<EntityVisSnapshot, MAX_VIS_ARMIES> states{};
        for (u32 a = 0; a < n; ++a) {
            states[a].vision =
                visibility_grid_->has_vision(pos.x, pos.z, a);
            states[a].radar =
                visibility_grid_->has_radar(pos.x, pos.z, a);
            states[a].sonar =
                visibility_grid_->has_sonar(pos.x, pos.z, a);
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

    // Build blip table: {_c_object = lightuserdata(entity)}
    lua_newtable(L_);
    int blip_tbl = lua_gettop(L_);
    lua_pushstring(L_, "_c_object");
    lua_pushlightuserdata(L_, entity);
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

} // namespace osc::sim
