#include "sim/collision_beam.hpp"

#include "core/dmath.hpp"
#include "core/test_status.hpp"
#include "map/terrain.hpp"
#include "sim/collision.hpp"
#include "sim/entity_registry.hpp"
#include "sim/projectile.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/weapon.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace osc::sim {

namespace {

Vector3 lerp(const Vector3& a, const Vector3& b, f32 t) {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
}

/// Put the beam on its launcher's muzzle, facing the way the muzzle does.
/// Returns that facing; nothing when its launcher is gone.
std::optional<Vector3> pose(EntityRegistry& registry, Entity& beam) {
    Entity* e = registry.find(beam.beam_launcher_id());
    if (!e || e->destroyed() || !e->is_unit()) return std::nullopt;
    const auto& launcher = static_cast<const Unit&>(*e);
    const i32 bone = beam.beam_setup().muzzle_bone;
    Vector3 dir = launcher.bone_world_forward(bone);
    const f32 len = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    dir = len > 1e-4f ? Vector3{dir.x / len, dir.y / len, dir.z / len} : Vector3{0, 0, 1};
    beam.set_position(launcher.bone_world_position(bone));
    const f32 across = std::sqrt(dir.x * dir.x + dir.z * dir.z);
    beam.set_orientation(
        euler_to_quat(osc::dmath::atan2(dir.x, dir.z), osc::dmath::atan2(-dir.y, across), 0.0f));
    return dir;
}

/// What retail's OnImpact calls the thing a beam met.
const char* impact_type(const Entity& hit) {
    if (hit.is_unit()) {
        const std::string& layer = static_cast<const Unit&>(hit).layer();
        if (layer == "Air") return "UnitAir";
        if (layer == "Sub" || layer == "Seabed") return "UnitUnderwater";
        return "Unit";
    }
    if (hit.is_shield()) return "Shield";
    if (hit.is_prop()) return "Prop";
    return "Projectile";
}

/// Whether `struck` lets the beam's weapon hit it: its
/// OnCollisionCheckWeapon(weapon), true without one (or without a weapon to
/// give it). An error counts as allowed, and fails a test run.
bool weapon_allowed(lua_State* L, const Entity& struck, int weapon_ref) {
    if (!L || struck.lua_table_ref() < 0 || weapon_ref < 0) return true;
    const int top = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, struck.lua_table_ref());
    lua_pushstring(L, "OnCollisionCheckWeapon");
    lua_gettable(L, -2);
    bool allowed = true;
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, top + 1);
        lua_rawgeti(L, LUA_REGISTRYINDEX, weapon_ref);
        if (lua_pcall(L, 2, 1, 0) == 0) {
            allowed = lua_toboolean(L, -1) != 0;
        } else {
            const char* err = lua_tostring(L, -1);
            const std::string message =
                std::string("OnCollisionCheckWeapon error: ") + (err ? err : "(unknown)");
            spdlog::warn("{}", message);
            if (test_status::count_lua_failures()) test_status::record_failure(message);
        }
    }
    lua_settop(L, top);
    return allowed;
}

/// The Lua table of the weapon that fires the beam, or LUA_NOREF.
int weapon_ref(const EntityRegistry& registry, const Entity& beam) {
    const Entity* e = registry.find(beam.beam_launcher_id());
    if (!e || e->destroyed() || !e->is_unit()) return LUA_NOREF;
    const auto& weapons = static_cast<const Unit*>(e)->weapons();
    const i32 index = beam.beam_setup().weapon;
    if (index < 0 || static_cast<size_t>(index) >= weapons.size()) return LUA_NOREF;
    return weapons[static_cast<size_t>(index)]->lua_table_ref;
}

} // namespace

void check_collision_beam(SimState& sim, lua_State* L, u32 beam_id, bool before_pass) {
    EntityRegistry& registry = sim.entity_registry();
    Entity* beam = registry.find(beam_id);
    if (!beam || beam->destroyed() || !beam->is_collision_beam()) return;
    const std::optional<Vector3> facing = pose(registry, *beam);
    if (!facing) return;
    auto& setup = beam->beam_setup();
    setup.check_clock = setup.check_interval + (before_pass ? 1 : 0);

    const u32 launcher_id = beam->beam_launcher_id();
    const f32 length = setup.length > 0 ? setup.length : 1.0f;
    const Vector3 from = beam->position();
    const Vector3 to{from.x + facing->x * length, from.y + facing->y * length,
                     from.z + facing->z * length};

    // The surface it would meet: the ground, or the water's from above.
    const map::Terrain* terrain = sim.terrain();
    f32 surface_t = 2.0f;
    const char* surface_type = nullptr;
    if (terrain) {
        if (const auto t = terrain_crossing(*terrain, from, to)) {
            surface_t = *t;
            surface_type = "Terrain";
        }
        const f32 water = terrain->water_elevation();
        if (terrain->has_water() && from.y >= water && to.y < water) {
            const f32 t = (from.y - water) / (from.y - to.y);
            const Vector3 at = lerp(from, to, t);
            if (t < surface_t && terrain->get_terrain_height(at.x, at.z) < water) {
                surface_t = t;
                surface_type = "Water";
            }
        }
    }

    // What stands in the way before it, nearest first. Scripted entities
    // (a flare's lure) and other beams don't stop a beam.
    std::vector<u32> candidates;
    registry.collect_colliders(from.x, from.z, to.x, to.z, candidates);
    std::vector<std::pair<f32, u32>> hits;
    for (const u32 id : candidates) {
        if (id == beam_id || id == launcher_id) continue;
        const Entity* e = registry.find(id);
        if (!e || e->destroyed() || e->is_collision_beam()) continue;
        if (!e->is_unit() && !e->is_prop() && !e->is_shield() && !e->is_projectile()) continue;
        if (e->is_unit()) {
            const auto* u = static_cast<const Unit*>(e);
            if (u->is_dying() || u->transport_id() != 0) continue;
        }
        if (e->is_projectile() && static_cast<const Projectile*>(e)->impacted) continue;
        // A shield stops what comes in, not what goes out.
        const auto t = segment_enters(e->collision_shape(), e->position(), e->orientation(), from,
                                      to, !e->is_shield());
        if (t && *t < surface_t) hits.emplace_back(*t, id);
    }
    std::sort(hits.begin(), hits.end());

    // The first that lets the weapon hit it (its script may say no: an ally
    // without CollideFriendly, a category on DoNotCollideList).
    u32 hit_id = 0;
    f32 hit_t = surface_t;
    const int weapon = weapon_ref(registry, *beam);
    for (const auto& [t, id] : hits) {
        const Entity* e = registry.find(id);
        if (!e || e->destroyed()) continue;
        const bool allowed = weapon_allowed(L, *e, weapon);
        // The check ran a script: the beam may be gone.
        beam = registry.find(beam_id);
        if (!beam || beam->destroyed()) return;
        e = registry.find(id);
        if (!allowed || !e || e->destroyed()) continue;
        hit_id = id;
        hit_t = t;
        break;
    }

    const char* type = nullptr;
    if (hit_id != 0) {
        type = impact_type(*registry.find(hit_id));
    } else if (surface_t <= 1.0f) {
        type = surface_type;
    } else {
        // Nothing in reach: it ends in the air, or under the water.
        hit_t = 1.0f;
        const bool under = terrain && terrain->has_water() && to.y < terrain->water_elevation() &&
                           terrain->get_terrain_height(to.x, to.z) < terrain->water_elevation();
        type = under ? "Underwater" : "Air";
    }
    Vector3 end = lerp(from, to, hit_t);
    if (hit_id == 0 && surface_type && surface_t <= 1.0f && std::string(surface_type) == "Water")
        end.y = terrain->water_elevation();
    beam->set_beam_endpoint(end);
    beam->beam_setup().reached = hit_t * length;

    // OnImpact(type, target): its script deals the damage.
    if (!L || beam->lua_table_ref() < 0) return;
    const int top = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, beam->lua_table_ref());
    lua_pushstring(L, "OnImpact");
    lua_gettable(L, -2);
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, top + 1);
        lua_pushstring(L, type);
        const Entity* hit = hit_id != 0 ? registry.find(hit_id) : nullptr;
        if (hit && hit->lua_table_ref() >= 0) lua_rawgeti(L, LUA_REGISTRYINDEX, hit->lua_table_ref());
        else lua_pushnil(L);
        if (lua_pcall(L, 3, 0, 0) != 0) {
            const char* err = lua_tostring(L, -1);
            const std::string message =
                std::string("CollisionBeam OnImpact error: ") + (err ? err : "(unknown)");
            spdlog::warn("{}", message);
            if (test_status::count_lua_failures()) test_status::record_failure(message);
        }
    }
    lua_settop(L, top);
}

void update_collision_beams(SimState& sim, lua_State* L) {
    EntityRegistry& registry = sim.entity_registry();
    auto& beams = sim.collision_beams();
    beams.erase(std::remove_if(beams.begin(), beams.end(),
                               [&](u32 id) {
                                   const Entity* e = registry.find(id);
                                   return !e || e->destroyed();
                               }),
                beams.end());
    const std::vector<u32> ids = beams; // a check's scripts may make or end beams
    for (const u32 id : ids) {
        Entity* beam = registry.find(id);
        if (!beam || beam->destroyed()) continue;
        const std::optional<Vector3> facing = pose(registry, *beam);
        if (!facing || !beam->beam_enabled()) continue;
        auto& setup = beam->beam_setup();
        if (setup.check_clock > 0) {
            --setup.check_clock;
            const Vector3 from = beam->position();
            beam->set_beam_endpoint({from.x + facing->x * setup.reached,
                                     from.y + facing->y * setup.reached,
                                     from.z + facing->z * setup.reached});
            continue;
        }
        check_collision_beam(sim, L, id, false);
    }
}

} // namespace osc::sim
