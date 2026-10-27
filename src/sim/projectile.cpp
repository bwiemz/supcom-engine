#include "sim/projectile.hpp"
#include "blueprints/blueprint_store.hpp"
#include "sim/sim_random.hpp"
#include "core/dmath.hpp"
#include "core/test_status.hpp"
#include "sim/collision.hpp"
#include "sim/entity_registry.hpp"
#include "sim/flight_math.hpp"
#include "sim/lua_sim.hpp"
#include "sim/unit.hpp"
#include "sim/weapon.hpp"
#include "map/terrain.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <spdlog/spdlog.h>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

namespace osc::sim {

namespace {

constexpr f32 kDegToRad = 0.017453292f;

/// The water's height, or Moho's -10000 on a map without
f32 water_line(const map::Terrain* terrain) {
    return terrain && terrain->has_water() ? terrain->water_elevation() : -10000.0f;
}

/// ReTargetOnMiss (Moho's Projectile::Impact, after OnImpact): while the target unit it was fired
/// at is still there, the weapon of `launcher` (`weapon`, -1 for none) hears whether the impact on
/// `hit_id` (0: the ground, the water, the air) of `type` struck it.
void report_impact(lua_State* L, EntityRegistry& registry, u32 launcher, i32 weapon, u32 fired_at,
                   u32 hit_id, const char* type) {
    if (weapon < 0 || !registry.find(fired_at)) return;
    Entity* e = registry.find(launcher);
    if (!e || e->destroyed() || !e->is_unit()) return;
    auto& owner = static_cast<Unit&>(*e);
    if (static_cast<size_t>(weapon) >= owner.weapons().size()) return;
    Weapon::TargetMark target;
    target.entity = fired_at;
    owner.weapons()[static_cast<size_t>(weapon)]->note_impact(
        owner, impact_missed(target, hit_id ? registry.find(hit_id) : nullptr, type, true),
        registry, get_sim(L));
}

/// Moho's CHeightField::GetNormal (faf-re STIMap.cpp, 0x0069A620): the gradient of the ground's
/// height half a unit either side of (x, z), normalized.
Vector3 ground_normal(const map::Terrain& terrain, f32 x, f32 z) {
    const f32 top = terrain.get_terrain_height(x, z - 0.5f);
    const f32 left = terrain.get_terrain_height(x - 0.5f, z);
    const f32 right = terrain.get_terrain_height(x + 0.5f, z);
    const f32 bottom = terrain.get_terrain_height(x, z + 0.5f);
    Vector3 n{left - right, 1.0f, top - bottom};
    const f32 length = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
    return {n.x / length, n.y / length, n.z / length};
}

} // namespace

// Helper: push a Vector3 as {[1]=x, [2]=y, [3]=z}
static void push_vec3(lua_State* L, const Vector3& v) {
    lua_newtable(L);
    lua_pushnumber(L, 1);
    lua_pushnumber(L, v.x);
    lua_settable(L, -3);
    lua_pushnumber(L, 2);
    lua_pushnumber(L, v.y);
    lua_settable(L, -3);
    lua_pushnumber(L, 3);
    lua_pushnumber(L, v.z);
    lua_settable(L, -3);
}

void Projectile::set_lifetime(f32 seconds, u32 now) {
    lifetime_end = now + static_cast<u32>(std::llrintf(seconds * 10.0f));
}

void Projectile::update(f64 dt, EntityRegistry& registry, lua_State* L, const map::Terrain* terrain,
                        u32 tick) {
    if (impacted) {
        // Lingering for its ImpactTimeout, it just goes.
        if (tick >= lifetime_end) {
            registry.destroy(*this);
        }
        return;
    }

    // Moho's MotionTick: a hit its last step found impacts now, in place of a step.
    if (impact_waiting()) {
        Entity* hit = impact_entity_id != 0 ? registry.find(impact_entity_id) : nullptr;
        const std::string type = impact_kind;
        impact_interpolation = -1.0f;
        impact_entity_id = 0;
        impact_kind.clear();
        on_impact(L, hit && !hit->destroyed() ? hit : nullptr, registry, terrain, type.c_str());
        return;
    }
    if (launch_tick == 0) {
        launch_tick = tick;
        launch_position = pending_position();
        launch_orientation = pending_orientation();
    }
    if (bounce_pending) {
        velocity = bounce_velocity;
        bounce_pending = false;
    }

    // Moho's MotionTick (faf-re Projectile.cpp): a shot thrusts the way it
    // faces. One that doesn't track falls (Moho runs the ballistic step only
    // `if (!mTrackTarget)`: FAF's torpedoes say UseGravity and sank to the
    // seabed under their sub), and with VelocityAlign turns after its
    // velocity; one that tracks turns toward its target first
    // (UpdateTracking). Its speed is capped, and it moves by the mean of its
    // old and new velocity (a shell's fall stays on the parabola its launch
    // angle was solved for).
    const auto step = static_cast<f32>(dt);
    const Vector3 start_velocity = velocity;
    const Vector3 from = pending_position();
    Quaternion facing = pending_orientation();
    const auto thrust = [&] {
        const Vector3 ahead = forward_of(facing);
        velocity.x += ahead.x * acceleration * step;
        velocity.y += ahead.y * acceleration * step;
        velocity.z += ahead.z * acceleration * step;
    };
    if (!tracking) {
        velocity.y += ballistic_accel * step;
        thrust();
        // TurnRate degrees a tick here, where UpdateTracking turns a tenth
        // of that: a shot with none keeps the facing it left with.
        if (velocity_align) facing = turned_toward(facing, velocity, turn_rate * kDegToRad);
    } else {
        if (!steer(facing, registry, L, terrain, tick)) return; // its script destroyed it
        thrust();
    }
    if (max_speed != 0.0f) {
        const f32 speed =
            std::sqrt(velocity.x * velocity.x + velocity.y * velocity.y + velocity.z * velocity.z);
        if (speed > max_speed) {
            const f32 cut = max_speed / speed;
            velocity = {velocity.x * cut, velocity.y * cut, velocity.z * cut};
        }
    }
    if (stay_upright) facing = coords_orient(forward_of(facing));

    auto pos = from;
    pos.x += (velocity.x + start_velocity.x) * step * 0.5f;
    pos.y += (velocity.y + start_velocity.y) * step * 0.5f;
    pos.z += (velocity.z + start_velocity.z) * step * 0.5f;

    // SetLocalAngularVelocity: it spins in its own frame, about its forward
    // axis if it aligns or tracks, its up axis if it stays upright, with the
    // rate it was given.
    const f32 spin_sq = angular_velocity.x * angular_velocity.x +
                        angular_velocity.y * angular_velocity.y +
                        angular_velocity.z * angular_velocity.z;
    if (spin_sq > 0.0f) {
        if (velocity_align || tracking || stay_upright) {
            const f32 rate = std::sqrt(spin_sq);
            angular_velocity =
                velocity_align || tracking ? Vector3{0.0f, 0.0f, rate} : Vector3{0.0f, rate, 0.0f};
        }
        const Vector3 turn{angular_velocity.x * step, angular_velocity.y * step,
                           angular_velocity.z * step};
        const f32 angle = std::sqrt(turn.x * turn.x + turn.y * turn.y + turn.z * turn.z);
        const f32 s = osc::dmath::sin(angle * 0.5f) / angle;
        facing = quat_multiply(
            facing, Quaternion{turn.x * s, turn.y * s, turn.z * s, osc::dmath::cos(angle * 0.5f)});
    }

    // A torpedo in the water stays just under its surface; one above it
    // (launched from a surfaced sub) is free to fall in (Moho clamps only
    // below the water, to a hundredth under).
    if (stay_underwater && in_water && terrain && pos.y > terrain->water_elevation() - 0.01f)
        pos.y = terrain->water_elevation() - 0.01f;
    set_position(pos);
    set_orientation(facing);

    // SetScaleVelocity: an effect that grows or shrinks as it flies.
    if (scale_velocity.x != 0 || scale_velocity.y != 0 || scale_velocity.z != 0) {
        const auto grown = [dt](f32 s, f32 v) {
            return std::max(0.0f, s + v * static_cast<f32>(dt));
        };
        set_scale(grown(scale_x(), scale_velocity.x), grown(scale_y(), scale_velocity.y),
                  grown(scale_z(), scale_velocity.z));
    }

    std::optional<PathImpact> impact = collide(from, registry, L, terrain);
    if (destroyed() || !in_registry()) return;

    // Air bursts (Moho's detonate band, which CheckCollision tests last and which
    // stands in place of anything else this tick met): flak on reaching its target's
    // height (DetonatesAtTargetHeight sets it), a shell coming down to its burst height.
    if (auto burst = detonate_band(from, terrain)) impact = burst;
    if (impact) {
        record_impact(impact->at, impact->interpolation, impact->hit, impact->type, terrain);
        return;
    }

    // Its time up, it bursts where this tick's move took it, an 'Air' (or
    // 'Underwater') impact: Moho's MotionTick tests the lifetime after
    // CheckCollision.
    if (tick >= lifetime_end) {
        const bool under = terrain && terrain->has_water() && pos.y < terrain->water_elevation();
        record_impact(pos, 1.0f, nullptr, under ? "Underwater" : "Air", terrain);
    }
}

bool Projectile::steer(Quaternion& facing, EntityRegistry& registry, lua_State* L,
                       const map::Terrain* terrain, u32 tick) {
    // Its aim: its target's aim spot (a unit's target point, anything
    // else's middle) while it has one, a ground target always. Once its
    // target is gone it has none: the script decides (Projectile.lua's
    // OnLostTarget shortens a homing shot's life) and it stops tracking,
    // steering this once more at the last aim if it was sent at something on
    // the ground or the water.
    bool has_target = has_target_position;
    if (target_entity_id > 0) {
        auto* target = registry.find(target_entity_id);
        if (target && !target->destroyed()) {
            target_position = target->is_unit()
                                  ? static_cast<const Unit*>(target)->target_point(target_point)
                                  : collision_centre(*target);
            has_target_position = true;
            has_target = true;
        } else {
            target_entity_id = 0;
            has_target_position = false;
            has_target = false;
        }
    }
    if (!has_target) {
        if (tracking) {
            if (!call_script(L, registry, "OnLostTarget")) return false;
            tracking = false;
        }
        if (!keep_last_aim) return true;
    }
    const Vector3 pos = pending_position();
    Vector3 aim = target_position;

    // LeadTarget: where its target will be by the time it gets there at its
    // top speed, the time found twice over, both times from the aim
    if (lead_target && max_speed > 0.0f && target_entity_id > 0) {
        if (const Entity* target = registry.find(target_entity_id)) {
            const Vector3 v = target->is_unit() ? static_cast<const Unit*>(target)->velocity()
                              : target->is_projectile()
                                  ? static_cast<const Projectile*>(target)->velocity
                                  : Vector3{};
            const Vector3 aim0 = aim;
            const auto lead_from = [&](const Vector3& at) {
                const f32 dx = pos.x - at.x, dy = pos.y - at.y, dz = pos.z - at.z;
                const f32 t = std::sqrt(dx * dx + dy * dy + dz * dz) / max_speed;
                return Vector3{aim0.x + v.x * t, aim0.y + v.y * t, aim0.z + v.z * t};
            };
            aim = lead_from(lead_from(aim0));
        }
    }

    // A torpedo aims at most a quarter under the surface, so one launched
    // above the water dives into it (Moho's underwater aim clamp, M206o).
    const f32 water = water_line(terrain);
    if (stay_underwater) aim.y = std::min(aim.y, water - 0.25f);

    Vector3 toward{aim.x - pos.x, aim.y - pos.y, aim.z - pos.z};

    // Zig-zag: a random offset, redrawn every ZigZagFrequency (in whole
    // ticks), weighted down within MaxZigZag of the aim, on a point its top
    // speed ahead; never under the ground, nor (unless it keeps under) the
    // water.
    if (max_zig_zag > 0.0f && zig_zag_freq > 0.0f) {
        if (zig_zag_next_tick <= tick) {
            SimRandom& rng = registry.sim_random();
            zig_zag_offset.x = rng.range(-max_zig_zag, max_zig_zag);
            zig_zag_offset.y = rng.range(-max_zig_zag, max_zig_zag);
            zig_zag_offset.z = rng.range(-max_zig_zag, max_zig_zag);
            zig_zag_next_tick = tick + static_cast<u32>(static_cast<i32>(zig_zag_freq * 10.0f));
        }
        const f32 dist = std::sqrt(toward.x * toward.x + toward.y * toward.y + toward.z * toward.z);
        const f32 blend = std::min(dist / max_zig_zag, 1.0f);
        const Vector3 dir =
            dist > 0.0f ? Vector3{toward.x / dist, toward.y / dist, toward.z / dist} : Vector3{};
        const f32 jx = pos.x + dir.x * max_speed + zig_zag_offset.x * blend;
        const f32 jz = pos.z + dir.z * max_speed + zig_zag_offset.z * blend;
        const f32 base_y = pos.y + dir.y * max_speed;
        f32 jy = base_y + zig_zag_offset.y * blend;
        const f32 ground = (terrain ? terrain->get_terrain_height(jx, jz) : 0.0f) + 0.5f;
        jy = std::max(jy, std::max(base_y, ground));
        if (!stay_underwater) jy = std::max(jy, std::min(base_y, water + 0.5f));
        toward = {jx - pos.x, jy - pos.y, jz - pos.z};
    }

    // It turns at most TurnRate degrees a second
    facing = turned_toward(facing, toward, turn_rate * kDegToRad * 0.1f);

    // Under the water, one rising toward the surface levels off at it
    if (stay_underwater && in_water) {
        const Vector3 ahead = forward_of(facing);
        if (ahead.y > 0.0f) {
            const f32 t = (water - pos.y) / ahead.y;
            if (t < 1.0f) {
                Vector3 clipped{ahead.x, ahead.y * t, ahead.z};
                const f32 len_sq =
                    clipped.x * clipped.x + clipped.y * clipped.y + clipped.z * clipped.z;
                if (len_sq < 1.0e-6f && len_sq > 0.0f) {
                    const f32 len = std::sqrt(len_sq);
                    clipped = {clipped.x / len, clipped.y / len, clipped.z / len};
                }
                facing = coords_orient(clipped);
            }
        }
    }

    // VelocityAlign: what of its velocity lies along its new facing
    if (velocity_align) {
        const Vector3 ahead = forward_of(facing);
        const f32 len_sq = ahead.x * ahead.x + ahead.y * ahead.y + ahead.z * ahead.z;
        const f32 along =
            len_sq > 0.0f
                ? (velocity.x * ahead.x + velocity.y * ahead.y + velocity.z * ahead.z) / len_sq
                : 0.0f;
        velocity = {ahead.x * along, ahead.y * along, ahead.z * along};
    }
    return true;
}

void Projectile::arm_lost_target_aim(const EntityRegistry& registry) {
    // Moho's Projectile constructor: a homing shot sent at something neither
    // in the air nor under the water steers on at where it last was.
    keep_last_aim = false;
    if (!tracking || target_entity_id == 0) return;
    const Entity* target = registry.find(target_entity_id);
    if (!target || !target->is_unit()) return;
    const std::string layer = static_cast<const Unit*>(target)->layer();
    keep_last_aim = layer != "Air" && layer != "Sub";
}

namespace {

Vector3 lerp(const Vector3& a, const Vector3& b, f32 t) {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
}

} // namespace

std::optional<Projectile::PathImpact> Projectile::collide(const Vector3& from,
                                                          EntityRegistry& registry, lua_State* L,
                                                          const map::Terrain* terrain) {
    if (!collision_enabled) return std::nullopt;
    const Vector3 to = pending_position();

    // The surface, with CollideSurface (Moho's CheckCollision): the water, then the ground.
    // Crossing the water's surface changes its layer and tells its script (a torpedo dropped
    // from the air starts tracking as it enters): retail's first, whatever else the step meets;
    // FAF's patched exe last, and not if the ground came first (FA-Binary-Patches
    // hooks/FixCollisions.cpp). A shell that DestroyOnWater ends where it crosses, as it goes
    // in (retail) or either way (FAF's); the ground, if nearer, instead.
    f32 surface_t = 2.0f;
    const char* surface_type = nullptr;
    if (terrain && collide_surface) {
        const bool faf = registry.faf_exe();
        const f32 water = terrain->water_elevation();
        enum class Crossing : u8 { None, Enter, Exit };
        Crossing crossing = Crossing::None;
        if (terrain->has_water()) {
            if (in_water) {
                if (to.y > water) crossing = Crossing::Exit;
            } else if (water > to.y) {
                crossing = Crossing::Enter;
            }
        }
        const auto change_layer = [&] {
            in_water = crossing == Crossing::Enter;
            return call_script(L, registry, in_water ? "OnEnterWater" : "OnExitWater");
        };
        if (crossing != Crossing::None && !faf && !change_layer()) return std::nullopt;
        // Along the step's line from where it was (0 when level with the water). One behind
        // the step's start ends nothing: Moho's interpolation there, below 0, is no impact.
        f32 water_t = 0.0f;
        if (crossing != Crossing::None) {
            const f32 dy = to.y - from.y;
            if (dy != 0.0f) water_t = (water - from.y) / dy;
            if (destroy_on_water && (faf || crossing == Crossing::Enter) && water_t >= 0.0f) {
                surface_t = water_t;
                surface_type = "Water";
            }
        }
        bool ground_first = false;
        if (const auto t = terrain_crossing(*terrain, from, to);
            t && (surface_t > 1.0f || *t < surface_t)) {
            surface_t = *t;
            surface_type = nullptr;
            ground_first = !(*t > water_t);
        }
        if (crossing != Crossing::None && faf && !ground_first && !change_layer())
            return std::nullopt;
    }

    // A tracking shot sent to a place (a strategic missile, which skims no
    // surface, or a missile whose target died) ends on reaching it.
    if (tracking && has_target_position && target_entity_id == 0) {
        const Vector3 d{to.x - from.x, to.y - from.y, to.z - from.z};
        const Vector3 w{target_position.x - from.x, target_position.y - from.y,
                        target_position.z - from.z};
        const f32 dd = d.x * d.x + d.y * d.y + d.z * d.z;
        const f32 t =
            dd > 0 ? std::clamp((w.x * d.x + w.y * d.y + w.z * d.z) / dd, 0.0f, 1.0f) : 0.0f;
        const Vector3 near = lerp(from, to, t);
        const f32 mx = target_position.x - near.x;
        const f32 my = target_position.y - near.y;
        const f32 mz = target_position.z - near.z;
        constexpr f32 kArrived = 1.0f;
        if (mx * mx + my * my + mz * mz <= kArrived * kArrived && t < surface_t) {
            surface_t = t;
            surface_type = nullptr;
        }
    }

    // What it meets on the way, nearest first, each asked again every tick. Retail's
    // CheckCollision sweeps a tenth of the step past each end (0.1 at 0x00E4F714), FAF's patched
    // exe a hundredth (FixCollisions), and measures the hit from the swept line's start.
    if (collide_entity) {
        const f32 kSweepBeyond = registry.faf_exe() ? 0.01f : 0.1f;
        const f32 kSweepLength = 1.0f + 2.0f * kSweepBeyond;
        const Vector3 d{to.x - from.x, to.y - from.y, to.z - from.z};
        const Vector3 start{from.x - d.x * kSweepBeyond, from.y - d.y * kSweepBeyond,
                            from.z - d.z * kSweepBeyond};
        const Vector3 end{to.x + d.x * kSweepBeyond, to.y + d.y * kSweepBeyond,
                          to.z + d.z * kSweepBeyond};
        std::vector<u32> candidates;
        registry.collect_colliders(start.x, start.z, end.x, end.z, candidates);
        std::vector<std::pair<f32, u32>> hits;
        for (const u32 id : candidates) {
            if (id == entity_id() || id == launcher_id) continue;
            const Entity* e = registry.find(id);
            if (!e || e->destroyed()) continue;
            // A corpse is in the way (Moho's CheckCollision); a unit in a hold isn't.
            if (e->is_unit() && static_cast<const Unit*>(e)->transport_id() != 0) {
                continue;
            }
            // A shield stops what comes in, not what goes out.
            const auto t = segment_enters(e->collision_shape(), e->position(), e->orientation(),
                                          start, end, !e->is_shield());
            if (t && *t * kSweepLength < surface_t) {
                hits.emplace_back(*t, id);
            }
        }
        std::sort(hits.begin(), hits.end());
        for (const auto& [t, id] : hits) {
            Entity* e = registry.find(id);
            if (!e || e->destroyed()) continue;
            if (!collision_allowed(L, registry, *e)) continue;
            e = registry.find(id);
            if (!e || e->destroyed()) continue;
            return PathImpact{lerp(start, end, t), t * kSweepLength, e, nullptr};
        }
    }

    if (surface_t > 1.0f) {
        return std::nullopt;
    }
    Vector3 at = lerp(from, to, surface_t);
    if (surface_type && terrain) {
        at.y = terrain->water_elevation();
    }
    return PathImpact{at, surface_t, nullptr, surface_type};
}

std::optional<Projectile::PathImpact> Projectile::detonate_band(const Vector3& from,
                                                                const map::Terrain* terrain) const {
    // CheckCollision's tail (Steam's exe 0x006A476E, faf-re Projectile.cpp): its height over
    // the ground or the water where the step ends (-10000 for no water); the burst point then
    // solved from the ground's height alone, a tenth of the step at the least, all of it at
    // the most. Retail reads the water whenever the map has some (0x006A3D0F); FAF's exe only
    // for a shot that CollideSurface, in its surface pass (0x0069D2B0).
    const Vector3 to = pending_position();
    const f32 ground = terrain ? terrain->get_terrain_height(to.x, to.z) : 0.0f;
    const bool faf = registry() && registry()->faf_exe();
    const f32 water = terrain && terrain->has_water() && (!faf || collide_surface)
                          ? terrain->water_elevation()
                          : -10000.0f;
    const f32 height = to.y - std::max(ground, water);
    const f32 above =
        detonate_above_override < 0.0f ? detonate_above_height : detonate_above_override;
    const f32 below =
        detonate_below_override < 0.0f ? detonate_below_height : detonate_below_override;
    const f32 dy = to.y - from.y;
    f32 f = 0.0f;
    if (below > 0.0f && below > height && dy < 0.0f) {
        f = (below + ground - from.y) / dy;
    } else if (above > 0.0f && height > above && dy > 0.0f) {
        f = (above + ground - from.y) / dy;
    } else {
        return std::nullopt;
    }
    f = f < 1.0f ? f : 1.0f; // NaN too
    f = std::max(f, 0.1f);
    return PathImpact{lerp(from, to, f), f, nullptr, "Air"};
}

void Projectile::record_impact(const Vector3& at, f32 interpolation, const Entity* hit,
                               const char* type, const map::Terrain* terrain) {
    const std::string kind = type ? type : impact_type(hit, terrain);
    // Moho's MotionTick: a hit of the ground (IMPACT_Terrain) counts, and one within its bounce
    // limit is no impact. Its velocity, damped, is reflected about the ground's normal there,
    // taken up next step, and it is set a twentieth of a unit back along the way it came.
    if (kind == "Terrain" && terrain && ground_hits++ < bounce_limit) {
        const Vector3 n = ground_normal(*terrain, at.x, at.z);
        const Vector3 d{velocity.x * bounce_damping, velocity.y * bounce_damping,
                        velocity.z * bounce_damping};
        const f32 two_dot = ((n.y * d.y + n.z * d.z) + n.x * d.x) * 2.0f;
        bounce_velocity = {d.x - n.x * two_dot, d.y - n.y * two_dot, d.z - n.z * two_dot};
        bounce_pending = true;
        Vector3 back{-velocity.x, -velocity.y, -velocity.z};
        const f32 length = std::sqrt(back.x * back.x + back.y * back.y + back.z * back.z);
        if (length > 0.0f) back = {back.x / length, back.y / length, back.z / length};
        else back = {};
        set_position({at.x + back.x * 0.05f, at.y + back.y * 0.05f, at.z + back.z * 0.05f});
        return;
    }
    set_position(at);
    impact_interpolation = interpolation;
    impact_entity_id = hit ? hit->entity_id() : 0;
    impact_kind = kind;
}

bool Projectile::collision_allowed(lua_State* L, EntityRegistry& registry, Entity& other) {
    if (!L) return true;
    const u32 self_id = entity_id();
    const u32 other_id = other.entity_id();
    // What is struck is asked, OnCollisionCheck(self, other) with `other`
    // the projectile; the projectile itself is not (faf-re Projectile::
    // CheckCollision calls only the collided entity's script). As Moho's
    // RunScript (0x0069F6B0), none, or an error in it, is no collision: a bare
    // Entity, whose class has none, isn't hit. (An entity of the engine's with
    // no script at all, which Moho can't have, is.) Retail's comment: "If we
    // return false the thing hitting us has no idea that it came into contact
    // with us". FAF's torpedo answers false to anything but an anti-torpedo
    // projectile hitting it, and had been asked about every ship it ran into.
    const auto check = [&](u32 self, u32 with) {
        const Entity* a = registry.find(self);
        const Entity* b = registry.find(with);
        if (!a || !b || a->destroyed() || b->destroyed()) return false;
        if (a->lua_table_ref() < 0 || b->lua_table_ref() < 0) return true;
        const int top = lua_gettop(L);
        lua_rawgeti(L, LUA_REGISTRYINDEX, a->lua_table_ref());
        lua_pushstring(L, "OnCollisionCheck");
        lua_gettable(L, -2);
        bool allowed = false;
        if (lua_isfunction(L, -1)) {
            lua_pushvalue(L, top + 1);
            lua_rawgeti(L, LUA_REGISTRYINDEX, b->lua_table_ref());
            if (lua_pcall(L, 2, 1, 0) == 0) {
                allowed = lua_toboolean(L, -1) != 0;
            } else {
                const char* err = lua_tostring(L, -1);
                const std::string message =
                    std::string("OnCollisionCheck error: ") + (err ? err : "(unknown)");
                spdlog::warn("{}", message);
                if (test_status::count_lua_failures()) test_status::record_failure(message);
            }
        }
        lua_settop(L, top);
        return allowed;
    };
    return check(other_id, self_id);
}

Projectile::BlueprintPhysics Projectile::apply_blueprint_physics(lua_State* L, SimRandom* rng) {
    BlueprintPhysics found;
    velocity_align = true;
    if (!L || blueprint_id().empty()) return found;
    const int top = lua_gettop(L);
    if (!blueprints::push_blueprint(L, blueprint_id())) return found;
    lua_pushstring(L, "Physics");
    lua_rawget(L, -2);
    if (!lua_istable(L, -1)) {
        lua_settop(L, top);
        return found;
    }
    const int physics = lua_gettop(L);
    const auto field = [&](const char* key) {
        lua_pushstring(L, key);
        lua_rawget(L, physics);
        return lua_type(L, -1);
    };
    const auto flag = [&](const char* key, bool& out) {
        if (field(key) == LUA_TBOOLEAN) out = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
    };
    const auto number = [&](const char* key, f32& out) {
        if (field(key) == LUA_TNUMBER) out = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
    };
    flag("VelocityAlign", velocity_align);
    flag("StayUpright", stay_upright);
    number("Acceleration", acceleration);
    number("MaxSpeed", max_speed);
    flag("TrackTarget", tracking);
    number("TurnRate", turn_rate);
    flag("LeadTarget", lead_target);
    number("MaxZigZag", max_zig_zag);
    number("ZigZagFrequency", zig_zag_freq);
    flag("StayUnderwater", stay_underwater);
    // Where it ends of itself: on the water (209 retail shells), or
    // bursting at a height above the surface.
    flag("DestroyOnWater", destroy_on_water);
    number("DetonateAboveHeight", detonate_above_height);
    number("DetonateBelowHeight", detonate_below_height);
    // Strategic missiles rise from their silos through the ground.
    flag("CollideSurface", collide_surface);
    // And some hit no entity (Moho's mDoCollision, from Physics.CollideEntity:
    // FAF's UEF build beams end on dummy projectiles made inside the unit
    // being built, which would otherwise hit it and be gone at once).
    flag("CollideEntity", collide_entity);
    if (field("UseGravity") == LUA_TBOOLEAN) found.use_gravity = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    found.realistic_ordinance =
        field("RealisticOrdinance") != LUA_TNIL && lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    number("Lifetime", found.lifetime);
    // Each made a little different: base +- its range, uniformly (Moho's
    // RandomSymmetricAround). Moho draws even for a range of 0; this only
    // for one there is.
    if (rng) {
        f32 spin = 0;
        f32 spin_range = 0;
        number("RotationalVelocity", spin);
        number("RotationalVelocityRange", spin_range);
        if (spin != 0.0f || spin_range != 0.0f) {
            Vector3 axis{gaussian(*rng), gaussian(*rng), gaussian(*rng)};
            const f32 length = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
            const f32 rate = (spin + rng->range(-spin_range, spin_range)) * kDegToRad / length;
            angular_velocity = {axis.x * rate, axis.y * rate, axis.z * rate};
        }
        const auto spread = [&](const char* key, f32& value) {
            f32 range = 0;
            number(key, range);
            if (range != 0.0f) value += rng->range(-range, range);
        };
        spread("TurnRateRange", turn_rate);
        spread("MaxSpeedRange", max_speed);
        spread("AccelerationRange", acceleration);
        spread("LifetimeRange", found.lifetime);
    }
    // Its bounces (Moho's RProjectileBlueprintPhysics defaults 0, 0 and 0.5): a pick in
    // [MinBounceCount, MaxBounceCount), as Moho scales one draw by the span. Moho draws for an
    // empty span too; this only for one there is, as above.
    f32 min_bounces = 0;
    f32 max_bounces = 0;
    number("MinBounceCount", min_bounces);
    number("MaxBounceCount", max_bounces);
    number("BounceVelDamp", bounce_damping);
    const auto low = static_cast<i32>(min_bounces);
    const auto span = static_cast<u32>(static_cast<i32>(max_bounces) - low);
    bounce_limit = low;
    if (rng && span != 0) {
        bounce_limit += static_cast<i32>((static_cast<u64>(span) * rng->next_u32()) >> 32);
    }
    lua_settop(L, top);
    return found;
}

const std::unordered_set<std::string>& Projectile::categories() const {
    // One with no blueprint (or none read yet) is just a projectile.
    static const std::unordered_set<std::string> kBare{"ALLPROJECTILES"};
    return info_ ? info_->categories : kBare;
}

const char* Projectile::impact_type(const Entity* target, const map::Terrain* terrain) const {
    const Vector3 pos = pending_position();
    const bool wet = terrain && terrain->has_water();
    const bool under = wet && (stay_underwater || pos.y < terrain->water_elevation() - 0.5f);
    if (target) {
        if (target->is_unit()) {
            const std::string& layer = static_cast<const Unit*>(target)->layer();
            if (layer == "Air") return "UnitAir";
            if (layer == "Sub" || layer == "Seabed") return "UnitUnderwater";
            return "Unit";
        }
        if (target->is_prop()) return "Prop";
        if (target->is_shield()) return "Shield";
        if (target->is_projectile()) return under ? "ProjectileUnderwater" : "Projectile";
        return "Unit";
    }
    if (wet && terrain->get_terrain_height(pos.x, pos.z) < terrain->water_elevation())
        return under ? "Underwater" : "Water";
    return "Terrain";
}

bool Projectile::call_script(lua_State* L, EntityRegistry& registry, const char* method) {
    const u32 id = entity_id();
    if (!L || lua_table_ref() < 0) return true;
    const int top = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, lua_table_ref());
    lua_pushstring(L, method);
    lua_gettable(L, -2);
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, top + 1);
        if (lua_pcall(L, 1, 0, 0) != 0) {
            const char* err = lua_tostring(L, -1);
            const std::string message =
                std::string("Projectile ") + method + " error: " + (err ? err : "(unknown)");
            spdlog::warn("{}", message);
            if (test_status::count_lua_failures()) test_status::record_failure(message);
        }
    }
    lua_settop(L, top);
    return registry.find(id) != nullptr;
}

void Projectile::on_impact(lua_State* L, Entity* target, EntityRegistry& registry,
                           const map::Terrain* terrain, const char* type_override) {
    if (!L) {
        registry.destroy(*this);
        return;
    }
    impacted = true;
    const u32 id = entity_id();
    const u32 target_id = target ? target->entity_id() : 0;
    const char* type = type_override ? type_override : impact_type(target, terrain);
    // Its scripts may destroy it: what ReTargetOnMiss's report needs, first. Moho reads the
    // shot's target as it lands (a script may have changed it); a bomb, which the engine sends at
    // no unit, keeps the one it was fired at.
    const u32 launcher = launcher_id;
    const i32 listener = impact_listener_weapon;
    const u32 fired_at = target_entity_id != 0 ? target_entity_id : impact_listener_target;

    bool scripted = false;
    if (lua_table_ref() >= 0) {
        const int top = lua_gettop(L);
        lua_rawgeti(L, LUA_REGISTRYINDEX, lua_table_ref());
        lua_pushstring(L, "OnImpact");
        lua_gettable(L, -2);
        scripted = lua_isfunction(L, -1);
        lua_settop(L, top);
    }
    // Moho's Projectile::Impact deals no damage: a scripted projectile's OnImpact deals it.
    if (!scripted || engine_damage) {
        deal_engine_damage(L, target, registry);
    }
    if (!scripted) report_impact(L, registry, launcher, listener, fired_at, target_id, type);

    if (scripted) {
        Entity* self = registry.find(id);
        if (!self) {
            return;
        }
        const int top = lua_gettop(L);
        lua_rawgeti(L, LUA_REGISTRYINDEX, self->lua_table_ref());
        lua_pushstring(L, "OnImpact");
        lua_gettable(L, -2);
        lua_pushvalue(L, top + 1);
        lua_pushstring(L, type);
        Entity* hit = target_id ? registry.find(target_id) : nullptr;
        if (hit && !hit->destroyed() && hit->lua_table_ref() >= 0)
            lua_rawgeti(L, LUA_REGISTRYINDEX, hit->lua_table_ref());
        else lua_pushnil(L);
        const bool ok = lua_pcall(L, 3, 0, 0) == 0;
        if (!ok) {
            const char* err = lua_tostring(L, -1);
            const std::string message =
                std::string("Projectile OnImpact error: ") + (err ? err : "(unknown)");
            spdlog::warn("{}", message);
            if (test_status::count_lua_failures()) test_status::record_failure(message);
        }
        lua_settop(L, top);
        report_impact(L, registry, launcher, listener, fired_at, target_id, type);
        if (ok) return; // the script destroys it, now or after its ImpactTimeout
    }

    // No script to finish it (or it broke): it goes now.
    if (Entity* self = registry.find(id)) {
        registry.destroy(*self);
    }
}

void Projectile::deal_engine_damage(lua_State* L, Entity* target, EntityRegistry& registry) {
    auto pos = pending_position();

    // Resolve launcher's Lua ref once (avoids stale pointer if launcher is
    // destroyed during DamageArea/Damage pcall in a future code path)
    int launcher_ref = -2; // LUA_NOREF
    if (launcher_id > 0) {
        Entity* launcher = registry.find(launcher_id);
        if (launcher && !launcher->destroyed() && launcher->lua_table_ref() >= 0)
            launcher_ref = launcher->lua_table_ref();
    }

    if (damage_radius > 0) {
        // Area damage: DamageArea(instigator, position, radius, damage,
        //                         damageType, damageFriendly)
        lua_pushstring(L, "DamageArea");
        lua_rawget(L, LUA_GLOBALSINDEX);
        if (lua_isfunction(L, -1)) {
            // instigator
            if (launcher_ref >= 0) {
                lua_rawgeti(L, LUA_REGISTRYINDEX, launcher_ref);
            } else {
                lua_pushnil(L);
            }
            // position
            push_vec3(L, pos);
            // radius
            lua_pushnumber(L, damage_radius);
            // damage
            lua_pushnumber(L, damage_amount);
            // damageType
            lua_pushstring(L, damage_type.c_str());
            // damageFriendly
            lua_pushboolean(L, 0);
            if (lua_pcall(L, 6, 0, 0) != 0) {
                spdlog::warn("Projectile DamageArea error: {}",
                             lua_tostring(L, -1));
                lua_pop(L, 1);
            }
        } else {
            lua_pop(L, 1);
        }
    } else if (target && !target->destroyed()) {
        // Single-target damage: Damage(instigator, target, amount, vector,
        //                               damageType) — FA canonical 5-arg form
        lua_pushstring(L, "Damage");
        lua_rawget(L, LUA_GLOBALSINDEX);
        if (lua_isfunction(L, -1)) {
            // instigator
            if (launcher_ref >= 0) {
                lua_rawgeti(L, LUA_REGISTRYINDEX, launcher_ref);
            } else {
                lua_pushnil(L);
            }
            // target
            if (target->lua_table_ref() >= 0) {
                lua_rawgeti(L, LUA_REGISTRYINDEX, target->lua_table_ref());
            } else {
                lua_pushnil(L);
            }
            // amount
            lua_pushnumber(L, damage_amount);
            // vector (impact position)
            push_vec3(L, pos);
            // damageType
            lua_pushstring(L, damage_type.c_str());
            if (lua_pcall(L, 5, 0, 0) != 0) {
                spdlog::warn("Projectile Damage error: {}",
                             lua_tostring(L, -1));
                lua_pop(L, 1);
            }
        } else {
            lua_pop(L, 1);
        }
    }
}

bool Projectile::has_live_target(const EntityRegistry& registry) const {
    if (target_entity_id == 0) return has_target_position;
    const Entity* target = registry.find(target_entity_id);
    if (!target || target->destroyed()) return false;
    return !target->is_unit() || !static_cast<const Unit*>(target)->is_dying();
}

} // namespace osc::sim
