// The sim's C++ state: the registry of entities, SimState's own fields,
// and the snapshot around them (M208c-b; see state_io.hpp).

#include "sim/state_io.hpp"
#include "map/terrain.hpp"

#include "map/pathfinder.hpp"
#include "map/pathfinding_grid.hpp"
#include "map/intel_grid.hpp"
#include "sim/army_brain.hpp"
#include "sim/bone_cache.hpp"
#include "sim/entity_registry.hpp"
#include "sim/manipulator.hpp"
#include "sim/projectile.hpp"
#include "sim/prop.hpp"
#include "sim/prop_script.hpp"
#include "sim/shield.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"

#include <algorithm>
#include <functional>

namespace osc::sim {

namespace {

constexpr char kMagic[8] = {'O', 'S', 'C', 'S', 'I', 'M', '0', '1'};
constexpr u32 kVersion = 39; // 2: entities' wanted loops (M216b); 3: emitter overrides (M214d);
                             // 4: jammers' fake blips (M215e); 5: intel handles (M215g);
                             // 6: weapons' lead physics;
                             // 7: unit cap costs, the army's cap exemption, build cap waits;
                             // 8: the scripts' consumption flag; 9: winged attack runs;
                             // 10: weapons' AutoInitiateAttackCommand;
                             // 11: NeedUnpack, orders' begun flag;
                             // 12: guards' fights and the GuardReturnRadius leash;
                             // 13: weapons' and aim controllers' YawOnlyOnTarget;
                             // 14: units' lifebars;
                             // 15: weapons' ground attacks (AttackGroundTries);
                             // 16: units' LayerChangeOffsetHeight;
                             // 17: hull facing (SlavedToBody, AttackAngle);
                             // 18: no engine veterancy XP;
                             // 19: platoons' unique names and DisbandOnIdle,
                             //     collision detectors' bone states;
                             // 20: aim spots (target points), BelowWaterTargetsOnly;
                             // 21: units' footprints (footprint classes);
                             // 22: launch rules (MuzzleVelocityRandom,
                             //     UseFiringSolutionInsteadOfAimBone, StraightDownOrdinance);
                             // 23: the occupation grid's occupants;
                             // 24: projectiles' flight (LeadTarget, zig-zag state,
                             //     the lost-target aim latch);
                             // 25: units' creation ticks (build templates' order);
                             // 26: Moho pathing (the switch, navigators' path state,
                             //     army path queues, path maps' dirty bits);
                             // 27: idle aircraft landing (AutoLandTime, the landing);
                             // 28: hovering aircraft's circling;
                             // 29: mobile units in the way (moved last tick, path owners);
                             // 30: formation orders' slots (formation layers);
                             // 31: jammers' fakes known fake (latched);
                             // 32: factory builds' counts (IncreaseBuildCountInQueue);
                             // 33: callbacks' Args as Lua data;
                             // 34: patrol legs' start;
                             // 35: reclaims' ticks before their first share;
                             // 36: silos' preset blocks (GiveNukeSiloAmmo(blocks, true));
                             // 37: builders' arm on target, and orders waiting for it;
                             // 38: builds' cleared sites, props being cleared and rebuilt wrecks;
                             // 39: the player's command ids issued, a formation's Move or FormMove
                             // 39: a builder's help to an unfinished unit is repairing
                             // 39: aircraft's lift
                             // 39: builder arms' tracking

// Past any game's ids (entities_ is indexed by id: a late game's runs to a
// few million, projectiles included).
constexpr u32 kMaxEntityId = 1u << 26;

// An entity's class, in the stream.
enum class EntityKind : u8 { Entity = 1, Unit, Projectile, Prop, Shield };

template <typename E> void enum8(StateWriter& w, E e) {
    w.u8v(static_cast<u8>(e));
}
template <typename E> E enum8(StateReader& r) {
    return static_cast<E>(r.u8v());
}

void save_ids(StateWriter& w, const std::vector<u32>& ids) {
    w.size(ids.size());
    for (u32 id : ids) w.u32v(id);
}
std::vector<u32> load_ids(StateReader& r) {
    std::vector<u32> ids(r.size(4));
    for (u32& id : ids) id = r.u32v();
    return ids;
}

// A map keyed by id, in id order (an unordered one sorted first).
template <typename Map, typename Save> void save_by_id(StateWriter& w, const Map& map, Save save) {
    std::vector<u32> ids;
    ids.reserve(map.size());
    for (const auto& kv : map) ids.push_back(kv.first);
    std::sort(ids.begin(), ids.end());
    w.size(ids.size());
    for (u32 id : ids) {
        w.u32v(id);
        save(map.at(id));
    }
}

} // namespace

// --------------------------------------------------------- EntityRegistry

void StateIO::save(StateWriter& w, const EntityRegistry& reg) {
    w.tag("REGY");
    // entities_ in id order, each with its class; live_, order_,
    // unit_order_, the grids and large_colliders_ follow from them.
    // graveyard_ is empty between ticks, removed_slots_ is order_'s own.
    std::vector<std::reference_wrapper<const Entity>> live;
    live.reserve(reg.live_count_);
    for (const auto& e : reg.entities_)
        if (e) live.emplace_back(*e);
    w.size(live.size());
    for (const Entity& e : live) {
        if (const auto* u = dynamic_cast<const Unit*>(&e)) {
            enum8(w, EntityKind::Unit);
            save(w, *u);
        } else if (const auto* p = dynamic_cast<const Projectile*>(&e)) {
            enum8(w, EntityKind::Projectile);
            save(w, *p);
        } else if (const auto* pr = dynamic_cast<const Prop*>(&e)) {
            enum8(w, EntityKind::Prop);
            save(w, *pr);
        } else if (const auto* s = dynamic_cast<const Shield*>(&e)) {
            enum8(w, EntityKind::Shield);
            save(w, *s);
        } else {
            enum8(w, EntityKind::Entity);
            save(w, e);
        }
    }
    w.u32v(reg.next_id_);
    // default_random_, sim_random_: the sim's generator (SimState's);
    // walking_: no walk is under way between ticks; unregister_hook_: the
    // sim's; grid_initialized_, grid_width_, grid_height_: the map's
}

void StateIO::load(StateReader& r, EntityRegistry& reg, SimState& sim) {
    r.tag("REGY");
    // What the boot made goes, unannounced: nothing of it was ever the
    // saved game's.
    reg.entities_.clear();
    reg.live_.clear();
    reg.live_count_ = 0;
    reg.order_.clear();
    reg.unit_order_.clear();
    reg.removed_slots_ = 0;
    reg.graveyard_.clear();
    reg.large_colliders_.clear();
    for (auto& cell : reg.grid_cells_) cell.clear();
    for (auto& cell : reg.unit_cells_) cell.clear();

    const size_t n = r.size(64);
    u32 last_id = 0;
    for (size_t i = 0; i < n && r.ok(); ++i) {
        std::unique_ptr<Entity> e;
        switch (enum8<EntityKind>(r)) {
        case EntityKind::Unit: {
            auto u = std::make_unique<Unit>();
            load(r, *u, sim);
            e = std::move(u);
            break;
        }
        case EntityKind::Projectile: {
            auto p = std::make_unique<Projectile>();
            load(r, *p);
            p->set_blueprint_info(sim.projectile_blueprint_info(p->blueprint_id()));
            e = std::move(p);
            break;
        }
        case EntityKind::Prop: {
            auto p = std::make_unique<Prop>();
            load(r, *p);
            if (auto* bones = sim.bone_cache())
                p->set_bone_data(bones->get(p->blueprint_id(), sim.lua_state()));
            read_prop_blueprint(sim.lua_state(), *p);
            e = std::move(p);
            break;
        }
        case EntityKind::Shield: {
            auto s = std::make_unique<Shield>();
            load(r, *s);
            e = std::move(s);
            break;
        }
        case EntityKind::Entity: {
            e = std::make_unique<Entity>();
            load(r, *e);
            break;
        }
        default: return r.fail("an entity of an unknown kind");
        }
        if (!r.ok()) return;
        const u32 id = e->entity_id();
        if (id <= last_id) return r.fail("entities out of id order");
        if (id > kMaxEntityId) return r.fail("an entity id past any game's");
        last_id = id;
        // As register_entity takes one, at its own id
        e->set_registry(&reg);
        e->set_grid_cell(-1, -1);
        reg.order_.push_back({id, e.get()});
        if (e->is_unit()) reg.unit_order_.push_back({id, e.get()});
        if (reg.entities_.size() <= id) reg.entities_.resize(id + 1);
        reg.live_.insert(e.get());
        reg.entities_[id] = std::move(e);
        ++reg.live_count_;
        Entity& placed = *reg.entities_[id];
        reg.notify_collision_shape_changed(placed);
        if (reg.grid_initialized_) {
            i32 cx = 0, cz = 0;
            reg.world_to_cell(placed.position().x, placed.position().z, cx, cz);
            reg.grid_insert(placed, cx, cz);
            placed.set_grid_cell(cx, cz);
        }
    }
    reg.next_id_ = r.u32v();
    if (r.ok() && reg.next_id_ <= last_id) r.fail("an entity id past the next one");
}

// ------------------------------------------------------------ SimState

void StateIO::save(StateWriter& w, const SimState& sim) {
    w.tag("SIMS");
    // L_: the host's; seed_: the setup's (and the recording's)
    w.u64v(sim.sim_random_.state());
    w.u64v(sim.seed_);
    save(w, sim.entity_registry_);
    save(w, sim.thread_manager_);
    // blueprint_store_: the host's; projectile_info_: a cache
    save_ids(w, sim.collision_beams_);
    save_ids(w, sim.ferry_beacons_);
    // checksum_trace_header_: the host's; tick_checksum_, tick_checksum_tick_,
    // tick_checksum_valid_: a memo
    w.size(sim.source_armies_.size());
    for (const auto& [source, army] : sim.source_armies_) {
        w.u32v(source);
        w.i32v(army);
    }
    w.i32v(sim.paused_by_);
    w.u32v(sim.pause_serial_);
    w.u32v(sim.resumed_serial_);
    w.size(sim.pause_timeouts_.size());
    for (const auto& [client, left] : sim.pause_timeouts_) {
        w.u32v(client);
        w.i32v(left);
    }
    // pause_holds_, issuing_source_: the host's; terrain_: the map's; pathfinding_grid_ and
    // pathfinder_: the map's, with occupied_footprints_ marked
    save_by_id(w, sim.occupied_footprints_, [&](const SimState::Footprint& f) {
        w.f32v(f.x);
        w.f32v(f.z);
        w.f32v(f.size_x);
        w.f32v(f.size_z);
    });
    // occupancy_: made again from what stands on it
    save_by_id(w, sim.ground_occupants_, [&](const GroundOccupant& g) {
        w.u8v(g.caps);
        w.size(g.rects.size());
        for (const OccupancyRect& r : g.rects) {
            w.i32v(r.x0);
            w.i32v(r.z0);
            w.i32v(r.x1);
            w.i32v(r.z1);
        }
    });
    // stored_to_destroy_: empty between ticks
    // The intel each entity has painted, where it painted it: the grids are
    // made again from it on a load (intel_grids_), as Moho's are.
    save_by_id(w, sim.painted_intel_, [&](const PaintedIntel& p) {
        w.i32v(p.army);
        for (const IntelHandle& h : p.handles) {
            w.u32v(h.radius);
            w.b(h.enabled);
            w.vec3(h.pos);
            w.u32v(h.last_tick);
        }
        w.vec3(p.last_pos);
        w.b(p.moved);
        w.u32v(p.pass);
    });
    w.u32v(sim.intel_pass_);
    // sound_manager_, tick_observer_, checksum_trace_, entity_trace_,
    // rng_trace_, rng_trace_from_, rng_trace_to_, rng_trace_draws_,
    // entity_trace_from_, entity_trace_to_: the host's; bone_cache_,
    // anim_cache_: caches
    {
        std::vector<std::string> armor;
        armor.reserve(sim.armor_def_.table_.size());
        for (const auto& kv : sim.armor_def_.table_) armor.push_back(kv.first);
        std::sort(armor.begin(), armor.end());
        w.size(armor.size());
        for (const auto& type : armor) {
            w.str(type);
            const auto& row = sim.armor_def_.table_.at(type);
            std::vector<std::pair<std::string, f32>> sorted(row.begin(), row.end());
            std::sort(sorted.begin(), sorted.end());
            w.size(sorted.size());
            for (const auto& [damage, mult] : sorted) {
                w.str(damage);
                w.f32v(mult);
            }
        }
    }
    save(w, sim.effect_registry_);
    save(w, sim.economy_events_);
    w.size(sim.armies_.size());
    for (const auto& a : sim.armies_) save(w, *a, sim);
    w.u32v(sim.tick_count_);
    // game_time_: tick_count_'s
    // post_loads_run_: the host's count of post-loads, not game state
    save(w, sim.command_scheduler_);
    w.u32v(sim.command_delay_);
    // local_command_sink_, local_callback_sink_, human_input_active_: the
    // host's; recorded_replay_, recording_, playback_, resume_tick_: the
    // host's too (a load adopts the saved game's history: adopt_history)
    w.size(sim.temp_visions_.size());
    for (const auto& v : sim.temp_visions_) {
        w.u32v(v.army);
        w.f32v(v.x);
        w.f32v(v.z);
        w.f32v(v.radius);
        w.i32v(v.remaining_ticks);
        w.b(v.painted);
    }
    w.size(sim.entity_intel_.size());
    for (const auto& [id, intel] : sim.entity_intel_) {
        w.u32v(id);
        w.i32v(intel.army);
        w.size(intel.sources.size());
        for (const auto& [type, src] : intel.sources) {
            w.str(type);
            w.f32v(src.radius);
            w.b(src.enabled);
        }
    }
    w.u32v(sim.next_command_id_);
    w.u32v(sim.player_commands_issued_);
    w.b(sim.game_ended_);
    w.b(sim.script_victory_);
    w.str(sim.victory_condition_);
    enum8(w, sim.victory_mode_);
    w.str(sim.share_condition_);
    enum8(w, sim.share_mode_);
    enum8(w, sim.fog_mode_);
    w.f32v(sim.no_rush_seconds_);
    w.f32v(sim.no_rush_radius_);
    w.b(sim.common_army_);
    w.b(sim.team_share_overflow_);
    // camera_shake_events_, death_events_, intel_flush_events_,
    // sound_requests_: the renderer's and the audio's, emptied each tick
    w.size(sim.resource_deposits_.size());
    for (const ResourceDeposit& d : sim.resource_deposits_) {
        w.f32v(d.x);
        w.f32v(d.y);
        w.f32v(d.z);
        w.f32v(d.size);
        enum8(w, d.type);
    }
    // build_ghost_bp_, build_ghost_foot_x_, build_ghost_foot_z_: the UI's
    w.f32v(sim.playable_x0_);
    w.f32v(sim.playable_z0_);
    w.f32v(sim.playable_x1_);
    w.f32v(sim.playable_z1_);
    w.b(sim.has_playable_rect_);
    // placement_rules_: a cache; s_sim_generation_: the process's
    save_by_id(w, sim.prev_entity_vis_, [&](const auto& snaps) {
        for (const SimState::EntityVisSnapshot& s : snaps) {
            w.b(s.vision);
            w.b(s.radar);
            w.b(s.sonar);
            w.b(s.omni);
        }
    });
    save_by_id(w, sim.los_ever_, [&](u32 bits) { w.u32v(bits); });
    w.size(sim.jam_offsets_.size()); // an ordered map: its own order
    for (const auto& [key, offsets] : sim.jam_offsets_) {
        w.u64v(key);
        w.size(offsets.size());
        for (const Vector3& o : offsets) w.vec3(o);
        const auto known = sim.jam_known_.find(key);
        w.u64v(known == sim.jam_known_.end() ? 0 : known->second);
    }
    save_by_id(w, sim.blip_cache_, [&](const auto& snaps) {
        for (const BlipSnapshot& s : snaps) {
            w.vec3(s.last_known_position);
            w.str(s.blueprint_id);
            w.i32v(s.entity_army);
            w.b(s.entity_dead);
        }
    });
    w.size(sim.blip_objects_.size());
    for (u32 id : sim.blip_objects_) w.u32v(id);
    const SimRandom& rng = sim.sim_random_;
    w.u8v(static_cast<u8>((sim.moho_pathing_ ? 1 : 0) | (rng.mt_ ? 2 : 0)));
    if (rng.mt_) {
        w.u32v(rng.mt_seed_);
        w.u64v(rng.mt_drawn_);
    }
    save_path_maps(w, sim);
}

void StateIO::load(StateReader& r, SimState& sim) {
    r.tag("SIMS");
    sim.sim_random_.state_ = r.u64v();
    sim.seed_ = r.u64v();
    sim.projectile_info_.clear(); // (the projectiles fill it again as they load)
    load(r, sim.entity_registry_, sim);
    load(r, sim.thread_manager_);
    sim.collision_beams_ = load_ids(r);
    sim.ferry_beacons_ = load_ids(r);
    sim.tick_checksum_valid_ = false;
    sim.source_armies_.clear();
    const size_t sources = r.size(8);
    for (size_t i = 0; i < sources; ++i) {
        const u32 source = r.u32v();
        sim.source_armies_[source] = r.i32v();
    }
    sim.paused_by_ = r.i32v();
    sim.pause_serial_ = r.u32v();
    sim.resumed_serial_ = r.u32v();
    sim.pause_timeouts_.clear();
    const size_t timeouts = r.size(8);
    for (size_t i = 0; i < timeouts; ++i) {
        const u32 client = r.u32v();
        sim.pause_timeouts_[client] = r.i32v();
    }
    // The boot's structures come off the pathfinding grid, the saved
    // game's go on.
    if (sim.pathfinding_grid_)
        for (const auto& [id, f] : sim.occupied_footprints_)
            sim.pathfinding_grid_->clear_obstacle(f.x, f.z, f.size_x, f.size_z);
    sim.occupied_footprints_.clear();
    const size_t footprints = r.size(20);
    for (size_t i = 0; i < footprints; ++i) {
        const u32 id = r.u32v();
        SimState::Footprint f{};
        f.x = r.f32v();
        f.z = r.f32v();
        f.size_x = r.f32v();
        f.size_z = r.f32v();
        sim.occupied_footprints_.emplace(id, f);
        if (sim.pathfinding_grid_)
            sim.pathfinding_grid_->mark_obstacle(f.x, f.z, f.size_x, f.size_z);
    }
    // The occupation grid: the boot's claims go, the saved game's come.
    sim.occupancy_ = sim.terrain_
                         ? OccupancyGrid(sim.terrain_->map_width(), sim.terrain_->map_height())
                         : OccupancyGrid();
    sim.ground_occupants_.clear();
    const size_t occupants = r.size(9);
    for (size_t i = 0; i < occupants; ++i) {
        const u32 id = r.u32v();
        GroundOccupant g;
        g.caps = r.u8v();
        g.rects.resize(r.size(16));
        for (OccupancyRect& c : g.rects) {
            c.x0 = r.i32v();
            c.z0 = r.i32v();
            c.x1 = r.i32v();
            c.z1 = r.i32v();
        }
        sim.occupy_ground(id, std::move(g));
    }
    // The places aircraft reserved to land on, from the aircraft.
    sim.entity_registry_.for_each_unit([&](Entity& e) {
        const OccupancyRect& rr = static_cast<Unit&>(e).idle_landing_.reserved;
        if (rr.x1 > rr.x0 && rr.z1 > rr.z0) sim.occupancy_.reserve(rr, true);
    });
    // path_tables_ is derived (roadmap item 4c): made again over the loaded
    // grid, every cluster dirty. No search reads them yet; when one does,
    // each map's dirty bits and scan cursor are game state (see
    // docs/plans/2026-10-07-per-class-pathing-design.md, 4c-2).
    sim.reset_path_tables();
    if (sim.pathfinding_grid_)
        sim.pathfinder_ = std::make_unique<map::Pathfinder>(*sim.pathfinding_grid_);
    sim.stored_to_destroy_.clear();
    sim.painted_intel_.clear();
    const size_t painted = r.size(4 + 4 + kIntelSources * 21 + 17);
    for (size_t i = 0; i < painted && r.ok(); ++i) {
        PaintedIntel& p = sim.painted_intel_[r.u32v()];
        p.army = r.i32v();
        for (IntelHandle& h : p.handles) {
            h.radius = r.u32v();
            h.enabled = r.b();
            h.pos = r.vec3();
            h.last_tick = r.u32v();
        }
        p.last_pos = r.vec3();
        p.moved = r.b();
        p.pass = r.u32v();
    }
    sim.intel_pass_ = r.u32v();
    sim.armor_def_.table_.clear();
    const size_t armor = r.size(8);
    for (size_t i = 0; i < armor && r.ok(); ++i) {
        auto& row = sim.armor_def_.table_[r.str()];
        const size_t damages = r.size(8);
        for (size_t k = 0; k < damages; ++k) {
            std::string damage = r.str();
            row[damage] = r.f32v();
        }
    }
    load(r, sim.effect_registry_);
    // unsettled_decals_: every decal restored, in the order it was made
    // (the settled ones drop out on the next look).
    sim.unsettled_decals_.clear();
    for (const auto& fx : sim.effect_registry_.all())
        if (fx && !fx->destroyed() && fx->decal()) sim.unsettled_decals_.push_back(fx->id());
    load(r, sim.economy_events_);
    if (r.size(64) != sim.armies_.size()) return r.fail("another count of armies");
    for (auto& a : sim.armies_) load(r, *a, sim);
    sim.tick_count_ = r.u32v();
    sim.game_time_ = sim.tick_count_ * SimState::SECONDS_PER_TICK;
    load(r, sim.command_scheduler_);
    sim.command_delay_ = r.u32v();
    sim.temp_visions_.resize(r.size(21));
    for (auto& v : sim.temp_visions_) {
        v.army = r.u32v();
        v.x = r.f32v();
        v.z = r.f32v();
        v.radius = r.f32v();
        v.remaining_ticks = r.i32v();
        v.painted = r.b();
    }
    sim.entity_intel_.clear();
    const size_t intel = r.size(12);
    for (size_t i = 0; i < intel && r.ok(); ++i) {
        auto& e = sim.entity_intel_[r.u32v()];
        e.army = r.i32v();
        const size_t n = r.size(9);
        for (size_t k = 0; k < n; ++k) {
            auto& src = e.sources[r.str()];
            src.radius = r.f32v();
            src.enabled = r.b();
        }
    }
    sim.next_command_id_ = r.u32v();
    sim.player_commands_issued_ = r.u32v();
    sim.game_ended_ = r.b();
    sim.script_victory_ = r.b();
    sim.victory_condition_ = r.str();
    sim.victory_mode_ = enum8<VictoryMode>(r);
    sim.share_condition_ = r.str();
    sim.share_mode_ = enum8<ShareMode>(r);
    sim.fog_mode_ = enum8<FogMode>(r);
    sim.no_rush_seconds_ = r.f32v();
    sim.no_rush_radius_ = r.f32v();
    sim.common_army_ = r.b();
    sim.team_share_overflow_ = r.b();
    sim.camera_shake_events_.clear();
    sim.death_events_.clear();
    sim.intel_flush_events_.clear();
    sim.sound_requests_.clear();
    sim.resource_deposits_.resize(r.size(17));
    for (ResourceDeposit& d : sim.resource_deposits_) {
        d.x = r.f32v();
        d.y = r.f32v();
        d.z = r.f32v();
        d.size = r.f32v();
        d.type = enum8<ResourceDeposit::Type>(r);
    }
    sim.playable_x0_ = r.f32v();
    sim.playable_z0_ = r.f32v();
    sim.playable_x1_ = r.f32v();
    sim.playable_z1_ = r.f32v();
    sim.has_playable_rect_ = r.b();
    sim.placement_rules_.clear();
    sim.prev_entity_vis_.clear();
    const size_t vis = r.size(4 + 4 * SimState::MAX_VIS_ARMIES);
    for (size_t i = 0; i < vis && r.ok(); ++i) {
        auto& snaps = sim.prev_entity_vis_[r.u32v()];
        for (SimState::EntityVisSnapshot& s : snaps) {
            s.vision = r.b();
            s.radar = r.b();
            s.sonar = r.b();
            s.omni = r.b();
        }
    }
    sim.los_ever_.clear();
    const size_t los = r.size(8);
    for (size_t i = 0; i < los; ++i) {
        const u32 id = r.u32v();
        sim.los_ever_[id] = r.u32v();
    }
    sim.jam_offsets_.clear();
    sim.jam_known_.clear();
    const size_t jammed = r.size(20);
    for (size_t i = 0; i < jammed && r.ok(); ++i) {
        const u64 key = r.u64v();
        auto& offsets = sim.jam_offsets_[key];
        offsets.resize(r.size(12));
        for (Vector3& o : offsets) o = r.vec3();
        if (const u64 known = r.u64v(); known != 0) sim.jam_known_[key] = known;
    }
    sim.blip_cache_.clear();
    const size_t blips = r.size(4 + 21 * SimState::MAX_VIS_ARMIES);
    for (size_t i = 0; i < blips && r.ok(); ++i) {
        auto& snaps = sim.blip_cache_[r.u32v()];
        for (BlipSnapshot& s : snaps) {
            s.last_known_position = r.vec3();
            s.blueprint_id = r.str();
            s.entity_army = r.i32v();
            s.entity_dead = r.b();
        }
    }
    sim.blip_objects_.clear();
    const size_t blip_objects = r.size(4);
    for (size_t i = 0; i < blip_objects; ++i) sim.blip_objects_.insert(r.u32v());
    // The grids, from what is painted (the fog of war, the armies and the
    // painted intel all loaded by now)
    sim.intel_grids_.reset();
    sim.build_intel_grids();
    const u8 switches = r.u8v();
    sim.moho_pathing_ = (switches & 1) != 0;
    SimRandom& rng = sim.sim_random_;
    rng.mt_ = (switches & 2) != 0;
    if (rng.mt_) {
        rng.mt_seed(r.u32v());
        rng.mt_drawn_ = r.u64v();
        rng.mt_engine_->discard(rng.mt_drawn_);
    }
    load_path_maps(r, sim);
}

// ------------------------------------------------------------- Snapshot

std::vector<u8> save_sim_state(const SimState& sim) {
    std::vector<u8> out;
    StateWriter w(out);
    w.raw(kMagic, sizeof kMagic);
    w.u32v(kVersion);
    StateIO::save(w, sim);
    w.tag("END.");
    return out;
}

std::string load_sim_state(SimState& sim, const std::vector<u8>& bytes) {
    StateReader r(bytes);
    char magic[sizeof kMagic] = {};
    r.raw(magic, sizeof magic);
    if (!r.ok() || std::memcmp(magic, kMagic, sizeof kMagic) != 0) return "not a sim snapshot";
    if (r.u32v() != kVersion) return "a sim snapshot of another version";
    StateIO::load(r, sim);
    r.tag("END.");
    if (r.ok() && !r.at_end()) r.fail("bytes after the snapshot");
    return r.ok() ? std::string() : r.error();
}

} // namespace osc::sim
