// The sim's state beside its entities: armies and their platoons and
// influence maps, effects, economy events, threads, and the orders still
// to run (M208c-b; see state_io.hpp).

#include "sim/state_io.hpp"

#include "map/terrain.hpp"
#include "sim/army_brain.hpp"
#include "sim/command_scheduler.hpp"
#include "sim/economy_event.hpp"
#include "sim/ieffect.hpp"
#include "sim/influence_map.hpp"
#include "sim/platoon.hpp"
#include "sim/sim_callback_queue.hpp"
#include "sim/sim_state.hpp"
#include "sim/thread_manager.hpp"

#include <algorithm>

namespace osc::sim {

namespace {

template <typename E> void enum8(StateWriter& w, E e) {
    w.u8v(static_cast<u8>(e));
}
template <typename E> E enum8(StateReader& r) {
    return static_cast<E>(r.u8v());
}
template <typename E> void enum32(StateWriter& w, E e) {
    w.i32v(static_cast<i32>(e));
}
template <typename E> E enum32(StateReader& r) {
    return static_cast<E>(r.i32v());
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

template <typename Map> void save_str_f64(StateWriter& w, const Map& map) {
    std::vector<std::pair<std::string, f64>> sorted(map.begin(), map.end());
    std::sort(sorted.begin(), sorted.end());
    w.size(sorted.size());
    for (const auto& [k, v] : sorted) {
        w.str(k);
        w.f64v(v);
    }
}
template <typename Map> Map load_str_f64(StateReader& r) {
    Map map;
    const size_t n = r.size(12);
    for (size_t i = 0; i < n; ++i) {
        std::string k = r.str();
        map[k] = r.f64v();
    }
    return map;
}

template <typename Map> void save_u32_str(StateWriter& w, const Map& map) {
    std::vector<std::pair<u32, std::string>> sorted(map.begin(), map.end());
    std::sort(sorted.begin(), sorted.end());
    w.size(sorted.size());
    for (const auto& [k, v] : sorted) {
        w.u32v(k);
        w.str(v);
    }
}
template <typename Map> Map load_u32_str(StateReader& r) {
    Map map;
    const size_t n = r.size(8);
    for (size_t i = 0; i < n; ++i) {
        const u32 k = r.u32v();
        map[k] = r.str();
    }
    return map;
}

void save_resource(StateWriter& w, const ResourceState& s) {
    w.f64v(s.income);
    w.f64v(s.requested);
    w.f64v(s.stored);
    w.f64v(s.max_storage);
    w.f64v(s.overflow);
}
ResourceState load_resource(StateReader& r) {
    ResourceState s;
    s.income = r.f64v();
    s.requested = r.f64v();
    s.stored = r.f64v();
    s.max_storage = r.f64v();
    s.overflow = r.f64v();
    return s;
}

void save_arg(StateWriter& w, const SimCallbackArg& a) {
    w.u8v(static_cast<u8>(a.index()));
    if (const auto* s = std::get_if<std::string>(&a)) w.str(*s);
    else if (const auto* n = std::get_if<f64>(&a)) w.f64v(*n);
    else w.b(std::get<bool>(a));
}
SimCallbackArg load_arg(StateReader& r) {
    switch (r.u8v()) {
    case 0: return r.str();
    case 1: return r.f64v();
    case 2: return r.b();
    default: r.fail("a callback argument of an unknown kind"); return false;
    }
}

} // namespace

// ------------------------------------------------------------- Platoon

void StateIO::save(StateWriter& w, const Platoon& p) {
    w.tag("PLAT");
    w.u32v(p.platoon_id_);
    w.i32v(p.army_index_);
    w.str(p.name_);
    w.i32v(p.lua_table_ref_);
    w.b(p.destroyed_);
    w.b(p.had_units_);
    w.str(p.plan_name_);
    save_ids(w, p.unit_ids_);
    save_u32_str(w, p.squad_map_);
    save_u32_str(w, p.formation_map_);
    w.str(p.formation_override_);
    w.i32v(p.priority_targets_ref_);
}

void StateIO::load(StateReader& r, Platoon& p) {
    r.tag("PLAT");
    p.platoon_id_ = r.u32v();
    p.army_index_ = r.i32v();
    p.name_ = r.str();
    p.lua_table_ref_ = r.i32v();
    p.destroyed_ = r.b();
    p.had_units_ = r.b();
    p.plan_name_ = r.str();
    p.unit_ids_ = load_ids(r);
    p.squad_map_ = load_u32_str<std::unordered_map<u32, std::string>>(r);
    p.formation_map_ = load_u32_str<std::unordered_map<u32, std::string>>(r);
    p.formation_override_ = r.str();
    p.priority_targets_ref_ = r.i32v();
}

// -------------------------------------------------------- InfluenceMap

void StateIO::save(StateWriter& w, const InfluenceMap& m) {
    w.tag("INFL");
    // grid_, width_, height_: from the map (checked on load)
    w.i32v(m.grid_);
    w.i32v(m.width_);
    w.i32v(m.height_);
    w.size(m.cells_.size());
    for (const InfluenceMap::Cell& c : m.cells_) {
        w.size(c.entries.size());
        for (const auto& [id, e] : c.entries) {
            w.u32v(id);
            w.i32v(e.source_army);
            w.vec3(e.position);
            w.f32v(e.source.air);
            w.f32v(e.source.surface);
            w.f32v(e.source.sub);
            w.f32v(e.source.economy);
            w.b(e.source.mobile);
            w.b(e.source.flies);
            w.b(e.source.mass_extractor);
            w.b(e.source.experimental);
            w.b(e.source.commander);
            enum8(w, e.layer);
            w.b(e.detailed);
            w.f32v(e.strength);
            w.f32v(e.decay);
            w.i32v(e.decay_ticks);
        }
        // threats: rebuilt each update, and read between (not derived)
        w.size(c.threats.size());
        for (const auto& lanes : c.threats)
            for (f32 v : lanes) w.f32v(v);
        for (f32 v : c.assigned) w.f32v(v);
        for (f32 v : c.assigned_decay) w.f32v(v);
    }
    // entry_cells_: from the cells' entries
}

void StateIO::load(StateReader& r, InfluenceMap& m) {
    r.tag("INFL");
    if (r.i32v() != m.grid_ || r.i32v() != m.width_ || r.i32v() != m.height_)
        return r.fail("an influence map of another map's size");
    if (r.size(1) != m.cells_.size()) return r.fail("an influence map of another map's size");
    m.entry_cells_.clear();
    for (size_t i = 0; i < m.cells_.size() && r.ok(); ++i) {
        InfluenceMap::Cell& c = m.cells_[i];
        c.entries.clear();
        const size_t entries = r.size(40);
        for (size_t k = 0; k < entries && r.ok(); ++k) {
            const u32 id = r.u32v();
            InfluenceMap::Entry& e = c.entries[id];
            e.source_army = r.i32v();
            e.position = r.vec3();
            e.source.air = r.f32v();
            e.source.surface = r.f32v();
            e.source.sub = r.f32v();
            e.source.economy = r.f32v();
            e.source.mobile = r.b();
            e.source.flies = r.b();
            e.source.mass_extractor = r.b();
            e.source.experimental = r.b();
            e.source.commander = r.b();
            e.layer = enum8<ThreatLayer>(r);
            e.detailed = r.b();
            e.strength = r.f32v();
            e.decay = r.f32v();
            e.decay_ticks = r.i32v();
            m.entry_cells_[id] = static_cast<i32>(i);
        }
        c.threats.resize(r.size(sizeof(f32) * c.assigned.size()));
        for (auto& lanes : c.threats)
            for (f32& v : lanes) v = r.f32v();
        for (f32& v : c.assigned) v = r.f32v();
        for (f32& v : c.assigned_decay) v = r.f32v();
    }
}

// ------------------------------------------------------------- ArmyBrain

void StateIO::save(StateWriter& w, const ArmyBrain& a) {
    w.tag("ARMY");
    w.i32v(a.index_);
    w.str(a.name_);
    w.str(a.nickname_);
    w.i32v(a.faction_);
    w.b(a.use_whole_map_);
    w.b(a.is_human_);
    w.b(a.is_civilian_);
    enum32(w, a.state_);
    w.b(a.ever_had_units_);
    w.i32v(a.lua_table_ref_);
    save_resource(w, a.economy_.mass);
    save_resource(w, a.economy_.energy);
    w.f64v(a.mass_efficiency_);
    w.f64v(a.event_mass_);
    w.f64v(a.event_energy_);
    w.f64v(a.energy_efficiency_);
    w.i32v(a.unit_cap_);
    w.b(a.ignore_unit_cap_);
    w.f64v(a.handicap_);
    w.f64v(a.bonus_storage_mass_);
    w.f64v(a.bonus_storage_energy_);
    {
        std::vector<std::pair<i32, Alliance>> sorted(a.alliances_.begin(), a.alliances_.end());
        std::sort(sorted.begin(), sorted.end(),
                  [](const auto& x, const auto& y) { return x.first < y.first; });
        w.size(sorted.size());
        for (const auto& [army, alliance] : sorted) {
            w.i32v(army);
            enum32(w, alliance);
        }
    }
    w.vec3(a.start_position_);
    w.b(a.influence_map_ != nullptr); // made when first asked for
    if (a.influence_map_) save(w, *a.influence_map_);
    w.i32v(a.current_enemy_index_);
    w.size(a.attack_vectors_.size());
    for (const auto& v : a.attack_vectors_) {
        w.vec3(v.position);
        w.vec3(v.direction);
    }
    w.b(a.has_color_);
    w.u8v(a.color_r_);
    w.u8v(a.color_g_);
    w.u8v(a.color_b_);
    w.str(a.current_plan_);
    w.b(a.resource_sharing_);
    w.str(a.skin_name_);
    {
        std::vector<std::string> sorted(a.build_restrictions_.begin(), a.build_restrictions_.end());
        std::sort(sorted.begin(), sorted.end());
        w.size(sorted.size());
        for (const auto& s : sorted) w.str(s);
    }
    save_str_f64(w, a.stats_);
    {
        std::vector<std::string> keys;
        keys.reserve(a.blueprint_stats_.size());
        for (const auto& kv : a.blueprint_stats_) keys.push_back(kv.first);
        std::sort(keys.begin(), keys.end());
        w.size(keys.size());
        for (const auto& k : keys) {
            w.str(k);
            save_str_f64(w, a.blueprint_stats_.at(k));
        }
    }
    w.size(a.stat_triggers_.size());
    for (const auto& t : a.stat_triggers_) {
        w.str(t.stat);
        w.str(t.name);
        w.str(t.compare);
        w.f64v(t.value);
        w.str(t.category);
    }
    w.size(a.platoons_.size());
    for (const auto& p : a.platoons_) save(w, *p);
    w.u32v(a.next_platoon_id_);
}

void StateIO::load(StateReader& r, ArmyBrain& a, const SimState& sim) {
    r.tag("ARMY");
    if (r.i32v() != a.index_) return r.fail("armies in another order");
    a.name_ = r.str();
    a.nickname_ = r.str();
    a.faction_ = r.i32v();
    a.use_whole_map_ = r.b();
    a.is_human_ = r.b();
    a.is_civilian_ = r.b();
    a.state_ = enum32<BrainState>(r);
    a.ever_had_units_ = r.b();
    a.lua_table_ref_ = r.i32v();
    a.economy_.mass = load_resource(r);
    a.economy_.energy = load_resource(r);
    a.mass_efficiency_ = r.f64v();
    a.event_mass_ = r.f64v();
    a.event_energy_ = r.f64v();
    a.energy_efficiency_ = r.f64v();
    a.unit_cap_ = r.i32v();
    a.ignore_unit_cap_ = r.b();
    a.handicap_ = r.f64v();
    a.bonus_storage_mass_ = r.f64v();
    a.bonus_storage_energy_ = r.f64v();
    a.alliances_.clear();
    const size_t alliances = r.size(8);
    for (size_t i = 0; i < alliances; ++i) {
        const i32 army = r.i32v();
        a.alliances_[army] = enum32<Alliance>(r);
    }
    a.start_position_ = r.vec3();
    a.influence_map_.reset();
    if (r.b()) {
        const map::Terrain* terrain = sim.terrain();
        if (!terrain) return r.fail("an influence map, and no terrain");
        a.influence_map_ = std::make_unique<InfluenceMap>(
            terrain->map_width(), terrain->map_height(), static_cast<u32>(sim.army_count()));
        load(r, *a.influence_map_);
    }
    a.current_enemy_index_ = r.i32v();
    a.attack_vectors_.resize(r.size(24));
    for (auto& v : a.attack_vectors_) {
        v.position = r.vec3();
        v.direction = r.vec3();
    }
    a.has_color_ = r.b();
    a.color_r_ = r.u8v();
    a.color_g_ = r.u8v();
    a.color_b_ = r.u8v();
    a.current_plan_ = r.str();
    a.resource_sharing_ = r.b();
    a.skin_name_ = r.str();
    a.build_restrictions_.clear();
    const size_t restrictions = r.size(4);
    for (size_t i = 0; i < restrictions; ++i) a.build_restrictions_.insert(r.str());
    a.stats_ = load_str_f64<std::unordered_map<std::string, f64>>(r);
    a.blueprint_stats_.clear();
    const size_t bp_stats = r.size(8);
    for (size_t i = 0; i < bp_stats; ++i) {
        std::string k = r.str();
        a.blueprint_stats_[k] = load_str_f64<std::map<std::string, f64>>(r);
    }
    a.stat_triggers_.clear();
    const size_t triggers = r.size(16);
    for (size_t i = 0; i < triggers && r.ok(); ++i) {
        StatTrigger t;
        t.stat = r.str();
        t.name = r.str();
        t.compare = r.str();
        t.value = r.f64v();
        t.category = r.str();
        a.stat_triggers_.push_back(std::move(t));
    }
    a.platoons_.clear();
    const size_t platoons = r.size(16);
    for (size_t i = 0; i < platoons && r.ok(); ++i) {
        auto p = std::make_unique<Platoon>();
        load(r, *p);
        a.platoons_.push_back(std::move(p));
    }
    a.next_platoon_id_ = r.u32v();
}

// ------------------------------------------------------------- Effects

void StateIO::save(StateWriter& w, const IEffectRegistry& fx) {
    w.tag("EFCT");
    w.size(fx.effects_.size());
    for (const auto& e : fx.effects_) {
        w.u32v(e->id_);
        enum8(w, e->type_);
        w.str(e->blueprint_path_);
        w.u32v(e->entity_id_);
        w.u32v(e->target_entity_id_);
        w.i32v(e->bone_index_);
        w.i32v(e->target_bone_index_);
        w.i32v(e->army_);
        w.f32v(e->scale_);
        w.f32v(e->offset_x_);
        w.f32v(e->offset_y_);
        w.f32v(e->offset_z_);
        w.b(e->destroyed_);
        w.i32v(e->lua_table_ref_);
        save_str_f64(w, e->params_);
        for (const f32 v : e->emitter_params_) w.f32v(v);
        w.u32v(e->emitter_params_set_);
        w.size(e->curve_ops_.size());
        for (const auto& op : e->curve_ops_) {
            w.u8v(op.curve);
            w.b(op.resize);
            w.f32v(op.a);
            w.f32v(op.b);
        }
        w.u32v(e->overrides_serial_);
        w.f64v(e->birth_time_);
        w.f64v(e->lifetime_);
        w.f64v(e->ends_at_);
        w.u32v(e->created_tick_);
        w.b(e->has_emitter_blueprint_);
        w.b(e->has_frame_);
        w.vec3(e->frame_position_);
        w.quat(e->frame_rotation_);
        w.b(e->decal_ != nullptr);
        if (e->decal_) {
            const DecalSpec& d = *e->decal_;
            w.b(d.splat);
            w.str(d.type);
            w.str(d.texture1);
            w.str(d.texture2);
            w.vec3(d.position);
            w.f32v(d.rotation_y);
            w.f32v(d.size_x);
            w.f32v(d.size_z);
            w.f32v(d.lod);
            w.u32v(d.remove_tick);
            w.i32v(d.army);
            w.u32v(d.fidelity);
        }
        w.u32v(e->seen_by_);
        w.f32v(e->light_size_);
        w.f32v(e->light_duration_);
        w.str(e->glow_texture_);
        w.str(e->ramp_texture_);
    }
    // by_id_: from effects_; lifetime_by_blueprint_: a cache
    w.u32v(fx.next_id_);
}

void StateIO::load(StateReader& r, IEffectRegistry& fx) {
    r.tag("EFCT");
    fx.effects_.clear();
    fx.by_id_.clear();
    const size_t n = r.size(64);
    for (size_t i = 0; i < n && r.ok(); ++i) {
        auto e = std::make_unique<IEffect>();
        e->id_ = r.u32v();
        e->type_ = enum8<EffectType>(r);
        e->blueprint_path_ = r.str();
        e->entity_id_ = r.u32v();
        e->target_entity_id_ = r.u32v();
        e->bone_index_ = r.i32v();
        e->target_bone_index_ = r.i32v();
        e->army_ = r.i32v();
        e->scale_ = r.f32v();
        e->offset_x_ = r.f32v();
        e->offset_y_ = r.f32v();
        e->offset_z_ = r.f32v();
        e->destroyed_ = r.b();
        e->lua_table_ref_ = r.i32v();
        e->params_ = load_str_f64<std::unordered_map<std::string, f64>>(r);
        for (f32& v : e->emitter_params_) v = r.f32v();
        e->emitter_params_set_ = r.u32v();
        e->curve_ops_.resize(r.size(10));
        for (auto& op : e->curve_ops_) {
            op.curve = r.u8v();
            op.resize = r.b();
            op.a = r.f32v();
            op.b = r.f32v();
        }
        e->overrides_serial_ = r.u32v();
        e->birth_time_ = r.f64v();
        e->lifetime_ = r.f64v();
        e->ends_at_ = r.f64v();
        e->created_tick_ = r.u32v();
        e->has_emitter_blueprint_ = r.b();
        e->has_frame_ = r.b();
        e->frame_position_ = r.vec3();
        e->frame_rotation_ = r.quat();
        if (r.b()) {
            auto d = std::make_shared<DecalSpec>();
            d->splat = r.b();
            d->type = r.str();
            d->texture1 = r.str();
            d->texture2 = r.str();
            d->position = r.vec3();
            d->rotation_y = r.f32v();
            d->size_x = r.f32v();
            d->size_z = r.f32v();
            d->lod = r.f32v();
            d->remove_tick = r.u32v();
            d->army = r.i32v();
            d->fidelity = r.u32v();
            e->decal_ = std::move(d);
        }
        e->seen_by_ = r.u32v();
        e->light_size_ = r.f32v();
        e->light_duration_ = r.f32v();
        e->glow_texture_ = r.str();
        e->ramp_texture_ = r.str();
        fx.by_id_[e->id_] = e.get();
        fx.effects_.push_back(std::move(e));
    }
    fx.next_id_ = r.u32v();
}

// ------------------------------------------------------- Economy events

void StateIO::save(StateWriter& w, const EconomyEventRegistry& ev) {
    w.tag("ECEV");
    w.size(ev.events_.size());
    for (const auto& e : ev.events_) {
        w.i32v(e->waiting_thread_ref_);
        w.u64v(e->waiting_thread_serial_);
        w.u32v(e->unit_id_);
        w.f64v(e->total_mass_);
        w.f64v(e->total_energy_);
        w.f64v(e->duration_);
        w.f64v(e->elapsed_);
        w.f64v(e->progress_);
        w.b(e->done_);
        w.b(e->cancelled_);
        w.b(e->counted_);
        w.i32v(e->lua_table_ref_);
        w.i32v(e->callback_ref_);
    }
}

void StateIO::load(StateReader& r, EconomyEventRegistry& ev) {
    r.tag("ECEV");
    ev.events_.clear();
    const size_t n = r.size(64);
    for (size_t i = 0; i < n && r.ok(); ++i) {
        const int waiting_ref = r.i32v();
        const u64 waiting_serial = r.u64v();
        const u32 unit = r.u32v();
        const f64 mass = r.f64v();
        const f64 energy = r.f64v();
        const f64 duration = r.f64v();
        auto e = std::make_unique<EconomyEvent>(unit, mass, energy, duration);
        e->waiting_thread_ref_ = waiting_ref;
        e->waiting_thread_serial_ = waiting_serial;
        e->elapsed_ = r.f64v();
        e->progress_ = r.f64v();
        e->done_ = r.b();
        e->cancelled_ = r.b();
        e->counted_ = r.b();
        e->lua_table_ref_ = r.i32v();
        e->callback_ref_ = r.i32v();
        ev.events_.push_back(std::move(e));
    }
}

// ----------------------------------------------------------------- Threads

void StateIO::save(StateWriter& w, const ThreadManager& t) {
    w.tag("THRD");
    // L_: the loading state's; coroutine: from lua_ref, once the Lua heap
    // is in (resolve_threads)
    w.size(t.threads_.size());
    for (const ThreadEntry& e : t.threads_) {
        w.i32v(e.lua_ref);
        w.i32v(e.wrapper_ref);
        w.i32v(e.wait_until_tick);
        w.b(e.dead);
        w.b(e.suspended);
        w.str(e.source);
        w.u64v(e.serial);
    }
    // pending_threads_, resuming_: empty and false between ticks
    w.u64v(t.next_serial_);
    w.i32v(t.instruction_budget_);
}

void StateIO::load(StateReader& r, ThreadManager& t) {
    r.tag("THRD");
    t.threads_.clear();
    t.pending_threads_.clear();
    t.resuming_ = false;
    t.threads_.resize(r.size(22));
    for (ThreadEntry& e : t.threads_) {
        e.coroutine = nullptr;
        e.lua_ref = r.i32v();
        e.wrapper_ref = r.i32v();
        e.wait_until_tick = r.i32v();
        e.dead = r.b();
        e.suspended = r.b();
        e.source = r.str();
        e.serial = r.u64v();
    }
    t.next_serial_ = r.u64v();
    t.instruction_budget_ = r.i32v();
}

// ------------------------------------------------------ Orders still to run

void StateIO::save(StateWriter& w, const SimCallbackEntry& c) {
    w.str(c.func_name);
    w.size(c.args.size());
    for (const auto& [k, v] : c.args) {
        w.str(k);
        save_arg(w, v);
    }
    w.b(c.value.has_value());
    if (c.value) save_arg(w, *c.value);
    save_ids(w, c.unit_ids);
}

void StateIO::load(StateReader& r, SimCallbackEntry& c) {
    c.func_name = r.str();
    c.args.clear();
    const size_t args = r.size(6);
    for (size_t i = 0; i < args; ++i) {
        std::string k = r.str();
        c.args[k] = load_arg(r);
    }
    c.value.reset();
    if (r.b()) c.value = load_arg(r);
    c.unit_ids = load_ids(r);
}

void StateIO::save(StateWriter& w, const ScheduledCommand& c) {
    w.u32v(c.exec_tick);
    w.u32v(c.source);
    w.u64v(c.sequence);
    save(w, c.command);
    save_ids(w, c.unit_ids);
    w.b(c.clear_existing);
    w.b(c.callback.has_value());
    if (c.callback) save(w, *c.callback);
}

void StateIO::load(StateReader& r, ScheduledCommand& c) {
    c.exec_tick = r.u32v();
    c.source = r.u32v();
    c.sequence = r.u64v();
    load(r, c.command);
    c.unit_ids = load_ids(r);
    c.clear_existing = r.b();
    c.callback.reset();
    if (r.b()) {
        c.callback.emplace();
        load(r, *c.callback);
    }
}

void StateIO::save(StateWriter& w, const CommandScheduler& s) {
    w.tag("SCHD");
    w.size(s.by_tick_.size());
    for (const auto& [tick, commands] : s.by_tick_) {
        w.u32v(tick);
        w.size(commands.size());
        for (const ScheduledCommand& c : commands) save(w, c);
    }
    // confirmed_frame_, lockstep_: the network's gate (the host's)
    save_ids(w, s.sources_);
    w.u64v(s.next_sequence_);
}

void StateIO::load(StateReader& r, CommandScheduler& s) {
    r.tag("SCHD");
    s.by_tick_.clear();
    const size_t ticks = r.size(8);
    for (size_t i = 0; i < ticks && r.ok(); ++i) {
        const u32 tick = r.u32v();
        auto& commands = s.by_tick_[tick];
        commands.resize(r.size(16));
        for (ScheduledCommand& c : commands) load(r, c);
    }
    s.sources_ = load_ids(r);
    s.next_sequence_ = r.u64v();
}

} // namespace osc::sim
