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
    if (const i32 weapon = beam.beam_setup().weapon;
        weapon >= 0 && static_cast<size_t>(weapon) >= launcher.weapons().size()) {
        return std::nullopt;
    }
    const BonePose at = attached_moho(
        {}, compose_moho({}, launcher.bone_world_transform(beam.beam_setup().muzzle_bone)));
    beam.set_position(at.position);
    beam.set_orientation(at.rotation);
    const Quaternion& q = at.rotation;
    return Vector3{(q.w * q.y + q.z * q.x) * 2.0f, (q.z * q.y - q.w * q.x) * 2.0f,
                   1.0f - (q.y * q.y + q.x * q.x) * 2.0f};
}

void reach_on(Entity& beam, const Vector3& facing) {
    const Vector3 from = beam.pending_position();
    const f32 reached = beam.beam_setup().reached;
    beam.set_beam_endpoint(
        {from.x + facing.x * reached, from.y + facing.y * reached, from.z + facing.z * reached});
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
/// OnCollisionCheckWeapon(weapon), no without one or on an error (Moho's
/// RunScript_OnCollisionCheckWeapon). With no script to ask, yes. An error fails a test run.
bool weapon_allowed(lua_State* L, const Entity& struck, int weapon_ref) {
    if (!L || struck.lua_table_ref() < 0 || weapon_ref < 0) return true;
    const int top = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, struck.lua_table_ref());
    lua_pushstring(L, "OnCollisionCheckWeapon");
    lua_gettable(L, -2);
    bool allowed = false;
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

/// The weapon that fires the beam, if it still has one.
const Weapon* beam_weapon(const EntityRegistry& registry, const Entity& beam) {
    const Entity* e = registry.find(beam.beam_launcher_id());
    if (!e || e->destroyed() || !e->is_unit()) return nullptr;
    const auto& weapons = static_cast<const Unit*>(e)->weapons();
    const i32 index = beam.beam_setup().weapon;
    if (index < 0 || static_cast<size_t>(index) >= weapons.size()) return nullptr;
    return weapons[static_cast<size_t>(index)].get();
}

/// The Lua table of the weapon that fires the beam, or LUA_NOREF.
int weapon_ref(const EntityRegistry& registry, const Entity& beam) {
    const Weapon* weapon = beam_weapon(registry, beam);
    return weapon ? weapon->lua_table_ref : LUA_NOREF;
}

/// How far a beam reaches, read at each check (Moho's UnitWeapon::CreateCollisionBeamHelper):
/// its weapon's MaximumBeamLength, else the weapon's MaxRadius as it is now, which
/// ChangeMaxRadius moves (a Cybran commander's range upgrade). A beam with no weapon reaches
/// the length it was made with.
f32 beam_reach(const EntityRegistry& registry, const Entity& beam) {
    if (const Weapon* w = beam_weapon(registry, beam)) {
        return w->max_beam_length > 0 ? w->max_beam_length : w->max_range;
    }
    const f32 length = beam.beam_setup().length;
    return length > 0 ? length : 1.0f;
}

/// How many times its reach a beam looks along itself (Moho's CreateCollisionBeamHelper,
/// Steam 0x006DD787): its reach over its reach across the ground, at most 5, and 1 when that
/// is 0.001 or less (straight up or down). A weapon's range is measured across the ground, so a
/// beam aimed up or down still covers it.
f32 sweep_stretch(const Vector3& from, const Vector3& facing, f32 reach) {
    const Vector3 end{facing.x * reach + from.x, facing.y * reach + from.y,
                      facing.z * reach + from.z};
    const f32 dx = from.x - end.x;
    const f32 dz = from.z - end.z;
    const f32 across = std::sqrt(dx * dx + dz * dz);
    if (!(across > 0.001f)) return 1.0f;
    return std::min(reach / across, 5.0f);
}

/// How far a beam from `from` that struck `hit` reaches (Moho's CreateCollisionBeamHelper):
/// in a straight line to a unit's centre (its bone -1, half its height above it) or to anything
/// else's position, not to where it entered them; to a shield, as far as where it met it
/// (`met` along the beam).
f32 struck_reach(const Entity& hit, const Vector3& from, f32 met) {
    if (hit.is_shield()) return met;
    const Vector3 at = hit.is_unit() ? hit.bone_world_transform(-1).position : hit.position();
    const f32 dx = from.x - at.x;
    const f32 dy = from.y - at.y;
    const f32 dz = from.z - at.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

} // namespace

void check_collision_beam(SimState& sim, lua_State* L, EntityId beam_id, bool before_step) {
    EntityRegistry& registry = sim.entity_registry();
    Entity* beam = registry.find(beam_id);
    if (!beam || beam->destroyed() || !beam->is_collision_beam()) return;
    const std::optional<Vector3> facing = pose(registry, *beam);
    if (!facing) return;
    auto& setup = beam->beam_setup();
    setup.check_clock = setup.check_interval + (before_step ? 1 : 0);

    const EntityId launcher_id = beam->beam_launcher_id();
    const Vector3 from = beam->pending_position();
    // It reaches its weapon's range along itself, but looks farther (Moho's
    // CreateCollisionBeamHelper): aimed up or down it still covers that range across the ground.
    const f32 reach = beam_reach(registry, *beam);
    const f32 sweep = reach * sweep_stretch(from, *facing, reach);
    const Vector3 to{from.x + facing->x * sweep, from.y + facing->y * sweep,
                     from.z + facing->z * sweep};

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

    // What stands in its way, nearest first. Scripted entities
    // (a flare's lure) and other beams don't stop a beam, nor do props: Moho's
    // beam gathers with the shots' line gather (GetEntityCollisionsInLine),
    // which walks none.
    std::vector<EntityId> candidates;
    registry.collect_line_colliders(from.x, from.z, to.x, to.z, candidates);
    std::vector<std::pair<f32, EntityId>> hits;
    for (const EntityId id : candidates) {
        if (id == beam_id || id == launcher_id) continue;
        const Entity* e = registry.find(id);
        if (!e || e->destroyed() || e->is_collision_beam()) continue;
        if (!e->is_unit() && !e->is_shield() && !e->is_projectile()) continue;
        if (e->is_unit() && static_cast<const Unit*>(e)->transport_id()) {
            continue;
        }
        const auto t =
            segment_enters(e->collision_shape(), e->position(), e->orientation(), from, to);
        if (t) hits.emplace_back(*t, id);
    }
    std::sort(hits.begin(), hits.end());

    // IgnoresAlly (Moho's UnitWeapon::GetClosestCollision, Steam's exe 0x006DD4C4): when its
    // weapon has it, a unit on the Air layer that isn't the enemy of the beam's unit doesn't
    // stop it (Moho's IsEnemy is no for an entity of no army).
    bool passes_allies = false;
    i32 owner_army = -1;
    if (const Entity* e = registry.find(launcher_id); e && !e->destroyed() && e->is_unit()) {
        const auto& owner = static_cast<const Unit&>(*e);
        const i32 index = setup.weapon;
        passes_allies = index >= 0 && static_cast<size_t>(index) < owner.weapons().size() &&
                        owner.weapons()[static_cast<size_t>(index)]->ignores_ally;
        owner_army = owner.army();
    }

    // The first that lets the weapon hit it (its script may say no: an ally
    // without CollideFriendly, a category on DoNotCollideList), then the test above.
    EntityId hit_id{};
    f32 hit_t = 0.0f;
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
        if (passes_allies && e->is_unit() && static_cast<const Unit*>(e)->is_air_unit() &&
            (e->army() < 0 || !sim.is_enemy(owner_army, e->army()))) {
            continue;
        }
        hit_id = id;
        hit_t = t;
        break;
    }

    // How far it reaches: to what it struck, else its reach. A surface nearer than that stops it
    // instead; Moho measures the surface against that length, so with nothing struck the ground
    // past its reach doesn't.
    f32 reached = reach;
    if (hit_id) {
        reached = struck_reach(*registry.find(hit_id), from, hit_t * sweep);
    }
    const char* type = nullptr;
    bool on_water = false;
    if (surface_type && surface_t * sweep < reached) {
        hit_id = {};
        reached = surface_t * sweep;
        type = surface_type;
        on_water = std::string(surface_type) == "Water";
    } else if (hit_id) {
        type = impact_type(*registry.find(hit_id));
    } else {
        // Nothing in reach: it ends in the air, or under the water.
        const Vector3 at{from.x + facing->x * reach, from.y + facing->y * reach,
                         from.z + facing->z * reach};
        const bool under = terrain && terrain->has_water() && at.y < terrain->water_elevation() &&
                           terrain->get_terrain_height(at.x, at.z) < terrain->water_elevation();
        type = under ? "Underwater" : "Air";
    }
    Vector3 end{from.x + facing->x * reached, from.y + facing->y * reached,
                from.z + facing->z * reached};
    if (on_water) {
        end.y = terrain->water_elevation();
    }
    beam->set_beam_endpoint(end);
    beam->beam_setup().reached = reached;

    // ReTargetOnMiss: its weapon hears whether the check struck the weapon's target (Moho's
    // CheckCollision, ahead of OnImpact). It runs no script.
    if (Entity* e = registry.find(launcher_id); e && !e->destroyed() && e->is_unit()) {
        auto& owner = static_cast<Unit&>(*e);
        const i32 index = setup.weapon;
        if (index >= 0 && static_cast<size_t>(index) < owner.weapons().size()) {
            Weapon& weapon = *owner.weapons()[static_cast<size_t>(index)];
            const Entity* hit = hit_id ? registry.find(hit_id) : nullptr;
            weapon.note_impact(owner, impact_missed(weapon.target_mark(), hit, type, false),
                               registry, &sim);
        }
    }

    // OnImpact(type, target): its script deals the damage.
    if (!L || beam->lua_table_ref() < 0) return;
    const int top = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, beam->lua_table_ref());
    lua_pushstring(L, "OnImpact");
    lua_gettable(L, -2);
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, top + 1);
        lua_pushstring(L, type);
        const Entity* hit = hit_id ? registry.find(hit_id) : nullptr;
        if (hit && hit->lua_table_ref() >= 0)
            lua_rawgeti(L, LUA_REGISTRYINDEX, hit->lua_table_ref());
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

bool step_collision_beam(SimState& sim, lua_State* L, Entity& beam) {
    if (beam.destroyed()) {
        return true;
    }
    const std::optional<Vector3> facing = pose(sim.entity_registry(), beam);
    if (!facing) {
        return false;
    }
    if (!beam.beam_enabled()) {
        return true;
    }
    auto& setup = beam.beam_setup();
    if (setup.check_clock > 0) {
        --setup.check_clock;
        reach_on(beam, *facing);
        return true;
    }
    check_collision_beam(sim, L, beam.entity_id(), false);
    return true;
}

void repose_collision_beam(EntityRegistry& registry, Entity& beam) {
    const std::optional<Vector3> facing = pose(registry, beam);
    if (facing && beam.beam_enabled()) {
        reach_on(beam, *facing);
    }
}

} // namespace osc::sim
