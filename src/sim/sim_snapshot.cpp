// A snapshot of the sim: its C++ state and its Lua heap (M208c-b; see
// sim_snapshot.hpp).

#include "sim/sim_snapshot.hpp"

#include "blueprints/blueprint_store.hpp"
#include "sim/army_brain.hpp"
#include "sim/economy_event.hpp"
#include "sim/manipulator.hpp"
#include "sim/platoon.hpp"
#include "sim/sim_state.hpp"
#include "sim/state_io.hpp"
#include "sim/thread_manager.hpp"
#include "sim/unit.hpp"
#include "sim/weapon.hpp"

#include <lpersist.h>
#include <spdlog/spdlog.h>

extern "C" {
#include <lua.h>
}

#include <algorithm>
#include <cstring>
#include <new>
#include <unordered_map>

namespace osc::sim {

namespace {

constexpr char kMagic[8] = {'O', 'S', 'C', 'S', 'N', 'A', 'P', '1'};
constexpr u32 kVersion = 1;

// What a light userdata is, in the Lua heap's snapshot.
enum Kind : u32 {
    K_ENTITY = 1, ///< id: the entity's
    K_WEAPON,     ///< id: its unit's << 32 | its index
    K_NAVIGATOR,  ///< id: its unit's
    K_MANIP,      ///< id: its unit's << 32 | its index
    K_ECONEVENT,  ///< id: its index among the events
    K_BRAIN,      ///< id: its army
    K_PLATOON,    ///< id: its army << 32 | its index
    K_HOST,       ///< id: its name's index in the snapshot's host names
    K_STALE,      ///< an address that names nothing live: NULL
};

u64 pair_id(u32 hi, u32 lo) {
    return (static_cast<u64>(hi) << 32) | lo;
}

// The snapshot's hash, written after it: a damaged one fails before anything
// of it is loaded.
u64 snapshot_hash(const u8* p, size_t n) {
    u64 h = 0xcbf29ce484222325ull;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        u64 w = 0;
        std::memcpy(&w, p + i, sizeof w);
        h = (h ^ w) * 0x9E3779B97F4A7C15ull;
        h ^= h >> 32;
    }
    for (; i < n; ++i) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

/// The light userdata held under string keys of the registry and of the
/// globals: the host's singletons, by name ("registry.osc_sim_state",
/// "global.osc_vfs").
std::vector<std::pair<std::string, void*>> host_pointers(lua_State* L) {
    std::vector<std::pair<std::string, void*>> out;
    const auto scan = [&](int table, const char* prefix) {
        lua_pushnil(L);
        while (lua_next(L, table) != 0) {
            if (lua_type(L, -2) == LUA_TSTRING && lua_type(L, -1) == LUA_TLIGHTUSERDATA)
                out.emplace_back(std::string(prefix) + lua_tostring(L, -2), lua_touserdata(L, -1));
            lua_pop(L, 1);
        }
    };
    scan(LUA_REGISTRYINDEX, "registry.");
    scan(LUA_GLOBALSINDEX, "global.");
    std::sort(out.begin(), out.end());
    return out;
}

/// Every blueprint's id and registry ref: a snapshot's refs name the saved
/// game's blueprint tables, and a load needs its own boot to have made the
/// same ones.
std::vector<std::pair<std::string, int>> blueprint_refs(const SimState& sim) {
    std::vector<std::pair<std::string, int>> out;
    const auto* store = sim.blueprint_store();
    if (!store) return out;
    using blueprints::BlueprintType;
    for (BlueprintType t :
         {BlueprintType::Unit, BlueprintType::Projectile, BlueprintType::Prop, BlueprintType::Mesh,
          BlueprintType::Beam, BlueprintType::Emitter, BlueprintType::TrailEmitter})
        for (const auto* e : store->get_all(t)) out.emplace_back(e->id, e->lua_ref);
    std::sort(out.begin(), out.end());
    return out;
}

struct Names {
    std::unordered_map<const void*, std::pair<u32, u64>> of;
    std::vector<std::string> hosts;
    u64 stale = 0;
};

struct Finder {
    SimState* sim = nullptr;
    std::vector<void*> events;
    std::vector<void*> hosts; ///< by the snapshot's host names (null: none here)
};

} // namespace

// The snapshot's work is StateIO's: it reaches private members.
std::string save_snapshot(SimState& sim, std::vector<u8>& out) {
    return StateIO::save_snapshot(sim, out);
}
std::string load_snapshot(SimState& sim, const std::vector<u8>& in) {
    return StateIO::load_snapshot(sim, in);
}

std::string StateIO::save_snapshot(SimState& sim, std::vector<u8>& out) {
    lua_State* L = sim.lua_state();
    if (!L) return "a sim without a Lua state";
    if (!sim.entity_registry_.graveyard_.empty() || !sim.thread_manager_.pending_threads_.empty() ||
        sim.thread_manager_.resuming_)
        return "not between ticks";

    // Every C++ object Lua may hold, by what it is
    Names names;
    sim.entity_registry_.for_each([&](Entity& e) {
        names.of[&e] = {K_ENTITY, e.entity_id()};
        auto* u = dynamic_cast<Unit*>(&e);
        if (!u) return;
        for (u32 i = 0; i < u->weapons_.size(); ++i)
            names.of[u->weapons_[i].get()] = {K_WEAPON, pair_id(u->entity_id(), i)};
        names.of[&u->navigator_] = {K_NAVIGATOR, u->entity_id()};
        for (u32 i = 0; i < u->manipulators_.size(); ++i)
            names.of[static_cast<const Waitable*>(u->manipulators_[i].get())] = {
                K_MANIP, pair_id(u->entity_id(), i)};
    });
    for (u32 i = 0; i < sim.economy_events_.events_.size(); ++i)
        names.of[static_cast<const Waitable*>(sim.economy_events_.events_[i].get())] = {K_ECONEVENT,
                                                                                        i};
    for (u32 a = 0; a < sim.armies_.size(); ++a) {
        names.of[sim.armies_[a].get()] = {K_BRAIN, a};
        const auto& platoons = sim.armies_[a]->platoons_;
        for (u32 i = 0; i < platoons.size(); ++i)
            names.of[platoons[i].get()] = {K_PLATOON, pair_id(a, i)};
    }
    for (const auto& [name, p] : host_pointers(L))
        if (names.of.emplace(p, std::make_pair(u32{K_HOST}, u64{names.hosts.size()})).second)
            names.hosts.push_back(name);

    lua_PersistHooks hooks;
    hooks.ud = &names;
    hooks.name = [](void* ud, void* p, std::uint32_t* kind, std::uint64_t* id) {
        auto* n = static_cast<Names*>(ud);
        auto it = n->of.find(p);
        if (it == n->of.end()) {
            ++n->stale;
            *kind = K_STALE;
            *id = 0;
            return true;
        }
        *kind = it->second.first;
        *id = it->second.second;
        return true;
    };

    std::string heap;
    if (std::string err = lua_persist(L, heap, hooks); !err.empty()) return "the Lua heap: " + err;
    // A handle outliving its object is NULL after a load; a kind of object
    // this map forgot would be too, so they are counted where they show
    if (names.stale > 0)
        spdlog::info("Snapshot: {} light userdata named nothing live (NULL after a load)",
                     names.stale);
    const std::vector<u8> state = save_sim_state(sim);

    const size_t start = out.size();
    StateWriter w(out);
    w.raw(kMagic, sizeof kMagic);
    w.u32v(kVersion);
    w.u32v(sim.tick_count());
    w.size(names.hosts.size());
    for (const auto& h : names.hosts) w.str(h);
    const auto refs = blueprint_refs(sim);
    w.size(refs.size());
    for (const auto& [id, ref] : refs) {
        w.str(id);
        w.i32v(ref);
    }
    w.u64v(state.size());
    w.raw(state.data(), state.size());
    w.u64v(heap.size());
    w.raw(heap.data(), heap.size());
    w.u64v(snapshot_hash(out.data() + start, out.size() - start));
    return {};
}

std::string StateIO::load_snapshot(SimState& sim, const std::vector<u8>& in) {
    lua_State* L = sim.lua_state();
    if (!L) return "a sim without a Lua state";
    if (in.size() < sizeof(kMagic) + sizeof(u64)) return "not a sim snapshot";
    u64 hash = 0;
    std::memcpy(&hash, in.data() + in.size() - sizeof hash, sizeof hash);
    if (snapshot_hash(in.data(), in.size() - sizeof hash) != hash) return "a damaged sim snapshot";
    const std::vector<u8> body(in.begin(), in.end() - static_cast<std::ptrdiff_t>(sizeof hash));
    StateReader r(body);
    char magic[sizeof kMagic] = {};
    r.raw(magic, sizeof magic);
    if (!r.ok() || std::memcmp(magic, kMagic, sizeof kMagic) != 0) return "not a sim snapshot";
    if (r.u32v() != kVersion) return "a sim snapshot of another version";
    const u32 tick = r.u32v();
    std::vector<std::string> host_names(r.size(4));
    for (auto& h : host_names) h = r.str();
    std::vector<std::pair<std::string, int>> refs(r.size(8));
    for (auto& [id, ref] : refs) {
        id = r.str();
        ref = r.i32v();
    }
    const u64 state_size = r.u64v();
    if (!r.ok() || state_size > r.left()) return "a truncated sim snapshot";
    std::vector<u8> state(static_cast<size_t>(state_size));
    r.raw(state.data(), state.size());
    const u64 heap_size = r.u64v();
    if (!r.ok() || heap_size > r.left()) return "a truncated sim snapshot";
    std::string heap(static_cast<size_t>(heap_size), '\0');
    r.raw(heap.data(), heap.size());
    if (!r.ok() || !r.at_end()) return "a malformed sim snapshot";
    if (sim.tick_count() != 0) return "a sim that has already run";
    if (refs != blueprint_refs(sim))
        return "the blueprints are not the saved game's (another build, mods or data)";

    // The loading host's singletons, before its heap goes
    Finder finder;
    finder.sim = &sim;
    {
        const auto here = host_pointers(L);
        for (const auto& name : host_names) {
            auto it =
                std::lower_bound(here.begin(), here.end(), std::make_pair(name, (void*)nullptr));
            finder.hosts.push_back(it != here.end() && it->first == name ? it->second : nullptr);
        }
    }

    try {
        if (std::string err = load_sim_state(sim, state); !err.empty())
            return "the sim's state: " + err;
    } catch (const std::bad_alloc&) {
        return "the sim's state: out of memory (a damaged snapshot?)";
    }
    if (sim.tick_count() != tick) return "a sim snapshot at odds with itself";
    for (const auto& e : sim.economy_events_.events_)
        finder.events.push_back(static_cast<Waitable*>(e.get()));

    lua_PersistHooks hooks;
    hooks.ud = &finder;
    hooks.find = [](void* ud, std::uint32_t kind, std::uint64_t id, void** p) {
        const auto* f = static_cast<Finder*>(ud);
        SimState& sim = *f->sim;
        const auto unit = [&](u32 uid) {
            return dynamic_cast<Unit*>(sim.entity_registry_.find(uid));
        };
        const u32 hi = static_cast<u32>(id >> 32);
        const u32 lo = static_cast<u32>(id);
        *p = nullptr;
        switch (kind) {
        case K_ENTITY: *p = sim.entity_registry_.find(lo); return *p != nullptr;
        case K_WEAPON: {
            Unit* u = unit(hi);
            if (!u || lo >= u->weapons_.size()) return false;
            *p = u->weapons_[lo].get();
            return true;
        }
        case K_NAVIGATOR: {
            Unit* u = unit(lo);
            if (!u) return false;
            *p = &u->navigator_;
            return true;
        }
        case K_MANIP: {
            Unit* u = unit(hi);
            if (!u || lo >= u->manipulators_.size()) return false;
            *p = static_cast<Waitable*>(u->manipulators_[lo].get());
            return true;
        }
        case K_ECONEVENT:
            if (id >= f->events.size()) return false;
            *p = f->events[id];
            return true;
        case K_BRAIN:
            if (id >= sim.armies_.size()) return false;
            *p = sim.armies_[id].get();
            return true;
        case K_PLATOON:
            if (hi >= sim.armies_.size() || lo >= sim.armies_[hi]->platoons_.size()) return false;
            *p = sim.armies_[hi]->platoons_[lo].get();
            return true;
        case K_HOST:
            if (id >= f->hosts.size()) return false;
            *p = f->hosts[id];
            return true;
        case K_STALE: return true;
        default: return false;
        }
    };
    if (std::string err = lua_unpersist(L, heap, hooks); !err.empty())
        return "the Lua heap: " + err;

    // Each thread's coroutine, from its registry ref
    for (ThreadEntry& t : sim.thread_manager_.threads_) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, t.lua_ref);
        t.coroutine = lua_type(L, -1) == LUA_TTHREAD ? lua_tothread(L, -1) : nullptr;
        lua_pop(L, 1);
        if (!t.coroutine && !t.dead) return "a thread whose coroutine is gone";
    }
    // Handles name this sim's generation (stale ones fail check_entity)
    const auto regenerate = [&](int ref) {
        if (ref < 0) return;
        lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
        if (lua_istable(L, -1)) {
            lua_pushstring(L, "_c_sim_gen");
            lua_rawget(L, -2);
            const bool has = !lua_isnil(L, -1);
            lua_pop(L, 1);
            if (has) {
                lua_pushstring(L, "_c_sim_gen");
                lua_pushnumber(L, SimState::sim_generation());
                lua_rawset(L, -3);
            }
        }
        lua_pop(L, 1);
    };
    sim.entity_registry_.for_each([&](Entity& e) { regenerate(e.lua_table_ref()); });
    for (const auto& a : sim.armies_) regenerate(a->lua_table_ref());
    lua_settop(L, 0);
    return {};
}

} // namespace osc::sim
