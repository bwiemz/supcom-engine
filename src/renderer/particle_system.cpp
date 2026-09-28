#include "renderer/particle_system.hpp"

#include "map/terrain.hpp"
#include "renderer/camera.hpp"
#include "renderer/frustum.hpp"
#include "renderer/recon_view.hpp"
#include "sim/world_snapshot.hpp"

#include <algorithm>
#include <cmath>

namespace osc::renderer {

namespace {

using sim::Quaternion;
using sim::Vector3;

Vector3 add(const Vector3& a, const Vector3& b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
Vector3 sub(const Vector3& a, const Vector3& b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
Vector3 scale(const Vector3& a, f32 s) {
    return {a.x * s, a.y * s, a.z * s};
}
Vector3 cross(const Vector3& a, const Vector3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
f32 length(const Vector3& a) {
    return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
}
Vector3 normalized(const Vector3& a) {
    const f32 len = length(a);
    return len > 1e-6f ? scale(a, 1.0f / len) : Vector3{};
}

/// Normalized lerp along the shorter arc (Moho's QuatLERP between ticks).
Quaternion nlerp(const Quaternion& a, const Quaternion& b, f32 t) {
    const f32 dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    const f32 s = dot < 0.0f ? -1.0f : 1.0f;
    Quaternion r{a.x + (s * b.x - a.x) * t, a.y + (s * b.y - a.y) * t, a.z + (s * b.z - a.z) * t,
                 a.w + (s * b.w - a.w) * t};
    const f32 len = std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z + r.w * r.w);
    if (len > 1e-8f) {
        r.x /= len;
        r.y /= len;
        r.z /= len;
        r.w /= len;
    }
    return r;
}

/// Frames an emitter keeps: its newest, and those a catch-up reaches back to.
constexpr size_t kFramesKept = ParticleSystem::MAX_CATCHUP + 2;
/// Moho's look at an emitter's intel: every fifth tick from its first.
constexpr u32 kLookEvery = 5;
/// CanSeeCam's sphere about the emitter.
constexpr f32 kViewRadius = 5.0f;
/// An EmitIfVisible emitter's position is refreshed every third tick.
constexpr u32 kPlaceEvery = 3;
/// Gravity, in units a tick² (EFFECT_USE_GRAVITY × 0.02).
constexpr f32 kGravity = 0.02f;
/// Degrees to radians, as Moho multiplies them.
constexpr f32 kDegToRad = 0.017453292f;
/// Where the water is on a map without it (Tick's -10000).
constexpr f32 kNoWater = -10000.0f;

/// How a particle's quad lies (the TRamp technique family its emitter
/// picks, UpdateCurve's tag).
enum class Lie : u8 { Billboard, Flat, Align, AlignToBone };

Lie lie_of(const EmitterBlueprintData& bp) {
    if (bp.align_rotation) return Lie::Align;
    if (bp.flat) return Lie::Flat;
    if (bp.align_to_bone) return Lie::AlignToBone;
    return Lie::Billboard;
}

} // namespace

f32 ParticleSystem::random() {
    u64 z = (random_state_ += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    return static_cast<f32>(z >> 40) * (1.0f / 16777216.0f);
}

void ParticleSystem::clear() {
    emitters_.clear();
    unknown_.clear();
    unmade_.clear();
    particles_.clear();
    last_tick_.reset();
    instances_.clear();
    groups_.clear();
    drawn_.clear();
}

std::vector<ParticleSystem::EmitterView> ParticleSystem::emitters() const {
    std::vector<EmitterView> out;
    out.reserve(emitters_.size());
    for (const auto& [id, e] : emitters_)
        out.push_back({id, e.bp->blueprint_id, e.position, e.clock, e.missed, e.seen});
    std::sort(out.begin(), out.end(),
              [](const EmitterView& a, const EmitterView& b) { return a.effect_id < b.effect_id; });
    return out;
}

bool ParticleSystem::can_see(Emitter& e, const sim::FrameView& view, u32 tick, const Vector3& eye,
                             const Frustum* frustum) const {
    const Vector3& p = e.position;
    if ((e.bp->lod_cutoff > 0 && length(sub(p, eye)) > e.bp->lod_cutoff) ||
        (frustum && !frustum->is_sphere_visible(p.x, p.y, p.z, kViewRadius))) {
        e.first_look.reset();
        return false;
    }
    if (!recon_) return true;
    // The player's army's LOS there, whoever made it: looked at on the
    // first tick it's in view and every fifth after.
    if (e.first_look && (tick - *e.first_look) % kLookEvery != 0) return e.seen;
    if (!e.first_look) e.first_look = tick;
    e.seen = recon_->sees_at(view, -1, p.x, p.z);
    return e.seen;
}

ParticleSystem::Frame ParticleSystem::frame_at(const Emitter& e, u32 ticks, f32 cursor) const {
    // `ticks` back, the tick runs from its older frame (cursor 0) to its
    // newer; a frame the emitter is too young to have is its oldest.
    const size_t n = e.frames.size();
    const auto at = [&](size_t back) { return e.frames[n - 1 - std::min(back, n - 1)]; };
    const Frame& older = at(ticks + 1);
    const Frame& newer = at(ticks);
    return {add(older.position, scale(sub(newer.position, older.position), cursor)),
            nlerp(older.rotation, newer.rotation, cursor)};
}

void ParticleSystem::emit(u32 id, Emitter& e, u32 ticks, u32 now_tick,
                          const map::Terrain* terrain) {
    const EmitterBlueprintData& bp = *e.bp;
    const auto phase_at = [&](f32 cursor) {
        // fmod with the sign of Repeattime; Repeattime 0 gives NaN, which
        // every curve reads as its first key, as Moho's does.
        const f32 repeat = bp.repeattime;
        f32 phase = std::fmod(e.clock - static_cast<f32>(ticks) + cursor, repeat);
        if ((phase < 0.0f) != (repeat < 0.0f)) phase += repeat;
        return phase;
    };
    const auto value = [&](EmitterCurveId c, f32 phase) {
        return bp.curve(c).value(phase, [this] { return random(); });
    };

    e.emissions += value(kEmitRate, phase_at(0.0f));
    const f32 whole = std::floor(e.emissions);
    e.emissions -= whole;
    const i32 count = static_cast<i32>(whole);
    if (count <= 0) return;
    const f32 step = bp.interpolate_emission ? 1.0f / static_cast<f32>(count) : 0.0f;
    const Frame fixed = frame_at(e, ticks, 0.0f);

    const bool water = terrain && terrain->has_water();
    const f32 water_y = water ? terrain->water_elevation() : kNoWater;
    for (i32 i = 0; i < count; ++i) {
        if (particles_.size() >= MAX_PARTICLES) break; // the buffer's full: the rest are lost
        const f32 cursor = static_cast<f32>(i) * step;
        const f32 phase = phase_at(cursor);
        const Frame f = bp.interpolate_emission ? frame_at(e, ticks, cursor) : fixed;
        // POSITION plus the position curves, in the emitter's frame.
        const Vector3 local{e.offset.x + value(kXPosition, phase) * e.scale,
                            e.offset.y + value(kYPosition, phase) * e.scale,
                            e.offset.z + value(kZPosition, phase) * e.scale};
        const Vector3 world = add(f.position, sim::quat_rotate(f.rotation, local));
        f32 y = world.y;
        if (bp.snap_to_waterline)
            y = bp.sort_order >= 0.0f ? std::max(water_y, world.y) : std::min(water_y, world.y);
        if (bp.only_emit_on_water) {
            const f32 ground = terrain ? terrain->get_terrain_height(world.x, world.z) : 0.0f;
            if (ground > water_y) continue;
            y = water_y;
        }

        Particle p;
        p.bp = e.bp;
        p.effect_id = id;
        // Scattered about its place, across the ground, by up to half its
        // Size either way.
        const f32 size = value(kSize, phase) * e.scale;
        const f32 r0 = random();
        const f32 r1 = random();
        const Vector3 spread = normalized({r1 - 0.5f, 0.0f, r0 - 0.5f});
        const f32 reach = (random() - 0.5f) * size;
        p.position = {world.x + spread.x * reach, y, world.z + spread.z * reach};

        p.acceleration = {value(kXAccel, phase) * e.scale, value(kYAccel, phase) * e.scale,
                          value(kZAccel, phase) * e.scale};
        if (bp.local_acceleration) p.acceleration = sim::quat_rotate(f.rotation, p.acceleration);
        if (bp.gravity) p.acceleration.y -= kGravity;

        Vector3 dir{value(kXDirection, phase) * e.scale, value(kYDirection, phase) * e.scale,
                    value(kZDirection, phase) * e.scale};
        if (bp.local_velocity) dir = sim::quat_rotate(f.rotation, dir);
        p.velocity = scale(dir, value(kVelocity, phase));
        p.resistance = value(kResistance, phase);
        p.lifetime = std::max(value(kLifetime, phase), 0.0f);
        p.begin_size = value(kStartSize, phase) * e.scale;
        p.end_size = value(kEndSize, phase) * e.scale;
        p.ramp_selection = value(kRampSelection, phase);
        p.framerate = value(kFrameRate, phase);
        p.texture_selection =
            std::floor(value(kTextureSelection, phase)) / std::max(bp.strip_count, 1.0f);
        if (!bp.align_to_bone) {
            p.angle = value(kInitialRotation, phase) * kDegToRad;
        } else {
            // Along the bone: a flat one turned to face its +Z, else moving
            // down it (its velocity the bone's axis).
            const Vector3 axis = sim::quat_rotate(f.rotation, {0.0f, 0.0f, 1.0f});
            if (bp.flat) {
                const Vector3 a = normalized(axis);
                p.angle = std::atan2(-a.x, a.z);
            } else {
                p.velocity = axis;
            }
        }
        p.spin = value(kRotationRate, phase) * kDegToRad;
        // Born `cursor` into its tick, on the render clock (where the
        // emitter's frame of tick k shows at time k + 1).
        p.born = static_cast<f64>(now_tick) - static_cast<f64>(ticks) + static_cast<f64>(cursor);
        particles_.push_back(p);
    }
}

void ParticleSystem::advance(const sim::FrameView& view, const Vector3& eye, const Frustum* frustum,
                             EmitterBlueprintCache& blueprints, lua_State* L,
                             const map::Terrain* terrain) {
    const sim::WorldSnapshot& snap = *view.cur();
    const u32 tick = snap.tick;
    const u32 steps = last_tick_ ? tick - *last_tick_ : 1;
    std::unordered_set<u32> live;
    for (const sim::EffectRecord& fx : snap.effects) {
        if (!fx.framed) continue;
        live.insert(fx.id);
        if (unknown_.count(fx.id) || unmade_.count(fx.id)) continue;
        auto it = emitters_.find(fx.id);
        const Frame frame{fx.frame_position, fx.frame_rotation};
        const Vector3 offset{fx.offset_x, fx.offset_y, fx.offset_z};
        if (it == emitters_.end()) {
            const EmitterBlueprintData* bp = blueprints.get(fx.blueprint_path, L);
            if (!bp) {
                unknown_.insert(fx.id);
                continue;
            }
            Emitter e;
            e.bp = bp;
            // Placed where it is made (Interpolate).
            e.position = add(frame.position, sim::quat_rotate(frame.rotation, offset));
            // CreateIfVisible: one the player couldn't see made never is.
            if (bp->create_if_visible && !can_see(e, view, tick, eye, frustum)) {
                unmade_.insert(fx.id);
                continue;
            }
            it = emitters_.emplace(fx.id, std::move(e)).first;
        }
        Emitter& e = it->second;
        e.frames.push_back(frame);
        if (e.frames.size() > kFramesKept) e.frames.pop_front();
        e.offset = offset;
        e.scale = fx.scale;
        // An EmitIfVisible emitter is placed afresh every third tick, where
        // its tick began.
        if ((e.bp->emit_if_visible || e.bp->create_if_visible) && tick % kPlaceEvery == 0) {
            const Frame start = frame_at(e, 0, 0.0f);
            e.position = add(start.position, sim::quat_rotate(start.rotation, offset));
        }
        if (e.bp->emit_if_visible && !can_see(e, view, tick, eye, frustum)) {
            e.missed += steps; // its clock stands while unseen
            continue;
        }
        // Ticks the sim ran that this view never showed (a slow frame) were
        // Moho's to emit, its clock running on: caught up with the missed.
        const u32 unshown = steps - 1;
        e.clock += static_cast<f32>(unshown);
        // Catch up the ticks it missed, as many as its particles live (and
        // 24); then this tick's (OnTick).
        const u32 back = std::min(
            {e.missed + unshown, MAX_CATCHUP, static_cast<u32>(std::max(e.bp->max_lifetime, 0))});
        for (u32 k = back; k > 0; --k) emit(fx.id, e, k, tick, terrain);
        e.missed = 0;
        emit(fx.id, e, 0, tick, terrain);
        e.clock += 1.0f; // TICKINCREMENT
    }
    // Emitters gone from the world stop; their particles live on.
    for (auto it = emitters_.begin(); it != emitters_.end();)
        it = live.count(it->first) ? std::next(it) : emitters_.erase(it);
    for (auto* set : {&unknown_, &unmade_})
        for (auto it = set->begin(); it != set->end();)
            it = live.count(*it) ? std::next(it) : set->erase(it);
    last_tick_ = tick;
}

void ParticleSystem::update(const sim::FrameView& view, const Camera& camera,
                            const Frustum* frustum, EmitterBlueprintCache& blueprints, lua_State* L,
                            const map::Terrain* terrain) {
    instances_.clear();
    groups_.clear();
    drawn_.clear();
    const sim::WorldSnapshot* cur = view.cur();
    if (!cur) return;

    f32 ex = 0;
    f32 ey = 0;
    f32 ez = 0;
    camera.eye_position(ex, ey, ez);
    const Vector3 eye{ex, ey, ez};
    const Vector3 forward =
        normalized(sub({camera.target_x(), camera.target_y(), camera.target_z()}, eye));
    // The view's right and up in the world (InverseViewMatrix rows 0, 1).
    const auto v = camera.view();
    const Vector3 right{v[0], v[4], v[8]};
    const Vector3 up{v[1], v[5], v[9]};

    if (last_tick_ && cur->tick < *last_tick_) clear(); // a new game
    if (!last_tick_ || cur->tick != *last_tick_)
        advance(view, eye, frustum, blueprints, L, terrain);

    // The render clock: the entity is drawn between its last two ticks.
    const f64 now = static_cast<f64>(cur->tick) + static_cast<f64>(view.alpha());
    std::erase_if(particles_, [now](const Particle& p) {
        return p.lifetime <= 0.0f || now >= p.born + static_cast<f64>(p.lifetime);
    });

    // Draw order: under the water first, then by SortOrder, textures and
    // blend (Moho's buckets), each bucket in the order it emitted; the
    // refracting ones apart, after them all (Moho's refracting buckets,
    // whatever their SortOrder; M214d).
    std::vector<const Particle*> order;
    order.reserve(particles_.size());
    for (const Particle& p : particles_)
        if (p.bp->blendmode >= 0 && p.bp->blendmode <= kBlendRefract) order.push_back(&p);
    std::stable_sort(order.begin(), order.end(), [](const Particle* a, const Particle* b) {
        const EmitterBlueprintData& x = *a->bp;
        const EmitterBlueprintData& y = *b->bp;
        const bool x_refracts = x.blendmode == kBlendRefract;
        if (x_refracts != (y.blendmode == kBlendRefract)) return !x_refracts;
        if (!x_refracts && (x.sort_order < 0) != (y.sort_order < 0)) return x.sort_order < 0;
        if (x.sort_order != y.sort_order) return x.sort_order < y.sort_order;
        if (x.texture != y.texture) return x.texture < y.texture;
        if (x.ramp_texture != y.ramp_texture) return x.ramp_texture < y.ramp_texture;
        return x.blendmode < y.blendmode;
    });

    instances_.reserve(order.size());
    for (const Particle* pp : order) {
        const Particle& p = *pp;
        const EmitterBlueprintData& bp = *p.bp;
        const f32 t = static_cast<f32>(now - p.born);
        const Lie lie = lie_of(bp);

        // WorldVS: its path from its spawn state; with drag (a resistance
        // r), (A/r² − V/r)(e^−rt − 1) + A·t/r + P.
        const bool drag = bp.particle_resistance && p.resistance > 1e-6f;
        Vector3 pos = p.position;
        Vector3 heading = add(p.velocity, scale(p.acceleration, t));
        if (drag) {
            const f32 inv = 1.0f / p.resistance;
            const f32 fall = std::exp(-p.resistance * t);
            if (lie != Lie::AlignToBone)
                pos = add(add(scale(sub(scale(p.acceleration, inv * inv), scale(p.velocity, inv)),
                                    fall - 1.0f),
                              scale(p.acceleration, inv * t)),
                          p.position);
            heading = add(scale(sub(p.velocity, scale(p.acceleration, inv)), fall),
                          scale(p.acceleration, inv));
        } else if (lie != Lie::AlignToBone) {
            pos = add(add(p.position, scale(p.velocity, t)), scale(p.acceleration, 0.5f * t * t));
        }
        const f32 size = p.begin_size + (p.end_size - p.begin_size) / p.lifetime * t;

        // Its quad: facing the camera, flat on the world, or along its
        // motion (WorldVSAlign), turned by its angle where it isn't aligned.
        Vector3 ax;
        Vector3 ay;
        if (lie == Lie::Align || lie == Lie::AlignToBone) {
            const Vector3 d = normalized(heading);
            ax = scale(normalized(cross(forward, d)), size);
            ay = scale(d, size);
        } else {
            const Vector3 r = lie == Lie::Flat ? Vector3{1.0f, 0.0f, 0.0f} : right;
            const Vector3 u = lie == Lie::Flat ? Vector3{0.0f, 0.0f, 1.0f} : up;
            const f32 turn = p.angle + p.spin * t;
            const f32 c = std::cos(turn);
            const f32 s = std::sin(turn);
            ax = scale(add(scale(r, c), scale(u, s)), size);
            ay = scale(sub(scale(u, c), scale(r, s)), size);
        }

        // Its texture: the whole of it, or (animated) frame floor(rate·t)
        // across and its selected strip down.
        std::array<f32, 4> uv{0.0f, 1.0f, 0.0f, 1.0f};
        if (bp.animated()) {
            const f32 frame = 1.0f / std::max(bp.frame_count, 1.0f);
            uv = {frame * std::floor(p.framerate * t), frame, p.texture_selection,
                  1.0f / std::max(bp.strip_count, 1.0f)};
        }
        ParticleInstance inst{};
        inst.center[0] = pos.x;
        inst.center[1] = pos.y;
        inst.center[2] = pos.z;
        inst.axis_x[0] = ax.x;
        inst.axis_x[1] = ax.y;
        inst.axis_x[2] = ax.z;
        inst.axis_y[0] = ay.x;
        inst.axis_y[1] = ay.y;
        inst.axis_y[2] = ay.z;
        std::copy(uv.begin(), uv.end(), inst.uv);
        inst.ramp[0] = t / p.lifetime;
        inst.ramp[1] = p.ramp_selection;
        const bool under = bp.sort_order < 0 && bp.blendmode != kBlendRefract;
        const auto offset = static_cast<u32>(instances_.size());
        instances_.push_back(inst);
        const bool same = !groups_.empty() && groups_.back().under_water == under &&
                          groups_.back().blendmode == bp.blendmode &&
                          groups_.back().texture == bp.texture &&
                          groups_.back().ramp == bp.ramp_texture;
        if (!same) groups_.push_back({under, bp.blendmode, bp.texture, bp.ramp_texture, offset, 0});
        ++groups_.back().count;
        drawn_.push_back(
            {p.effect_id, pos, ax, ay, t, p.lifetime, uv, p.ramp_selection, bp.blendmode, under});
    }
}

} // namespace osc::renderer
