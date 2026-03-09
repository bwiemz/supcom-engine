#include "renderer/particle_system.hpp"

#include "sim/entity.hpp"
#include "sim/ieffect.hpp"
#include "sim/sim_state.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace osc::renderer {

// ---------------------------------------------------------------------------
// sync_effects -- mirror IEffectRegistry into emitter state
// ---------------------------------------------------------------------------

void ParticleSystem::sync_effects(const sim::SimState& sim,
                                  EmitterBlueprintCache& bp_cache,
                                  lua_State* L) {
    const auto& effects = sim.effect_registry().all();

    // Mark emitters whose effect was destroyed
    for (auto& es : emitters_) {
        bool found = false;
        for (const auto& fx : effects) {
            if (fx && !fx->destroyed() && fx->id() == es.effect_id) {
                found = true;
                break;
            }
        }
        if (!found) es.active = false;
    }

    // Remove inactive emitters with no live particles
    emitters_.erase(
        std::remove_if(emitters_.begin(), emitters_.end(),
                       [](const EmitterState& es) {
                           return !es.active && es.particles.empty();
                       }),
        emitters_.end());

    // Create emitters for new effects
    for (const auto& fx : effects) {
        if (!fx || fx->destroyed()) continue;

        // Only emitter types
        auto t = fx->type();
        if (t != sim::EffectType::EMITTER_AT_ENTITY &&
            t != sim::EffectType::EMITTER_AT_BONE &&
            t != sim::EffectType::ATTACHED_EMITTER) {
            continue;
        }

        // Already tracked?
        bool exists = false;
        for (const auto& es : emitters_) {
            if (es.effect_id == fx->id()) { exists = true; break; }
        }
        if (exists) continue;

        // Load blueprint
        const auto* bp = bp_cache.get(fx->blueprint_path(), L);
        if (!bp) continue;

        EmitterState es;
        es.blueprint = bp;
        es.effect_id = fx->id();
        es.origin_x = fx->offset_x();
        es.origin_y = fx->offset_y();
        es.origin_z = fx->offset_z();
        emitters_.push_back(std::move(es));
    }

    // Update positions for attached emitters
    for (auto& es : emitters_) {
        if (!es.active) continue;
        for (const auto& fx : effects) {
            if (fx && fx->id() == es.effect_id && !fx->destroyed()) {
                // Get entity position if entity is attached
                if (fx->entity_id() > 0) {
                    auto* ent = sim.entity_registry().find(fx->entity_id());
                    if (ent) {
                        es.origin_x = ent->position().x + fx->offset_x();
                        es.origin_y = ent->position().y + fx->offset_y();
                        es.origin_z = ent->position().z + fx->offset_z();
                    }
                }
                break;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// emit_particles -- spawn new particles from emitter curves
// ---------------------------------------------------------------------------

void ParticleSystem::emit_particles(EmitterState& es, f32 dt) {
    if (!es.active || !es.blueprint) return;
    const auto& bp = *es.blueprint;

    // Emitter time fraction (0..1 over emitter lifetime)
    f32 t = (bp.lifetime > 0) ? (es.emitter_time / bp.lifetime) : 0.0f;
    t = std::clamp(t, 0.0f, 1.0f);

    // Emit rate at current emitter time
    f32 rate = bp.emit_rate.sample(t);
    if (rate <= 0) return;

    es.emit_accumulator += rate * dt;

    u32 total = particle_count();
    while (es.emit_accumulator >= 1.0f && total < MAX_PARTICLES) {
        ++total;
        es.emit_accumulator -= 1.0f;

        Particle p;

        // Position offset from curves
        f32 px = bp.x_pos.sample_random(t);
        f32 py = bp.y_pos.sample_random(t);
        f32 pz = bp.z_pos.sample_random(t);
        p.pos_x = es.origin_x + px;
        p.pos_y = es.origin_y + py;
        p.pos_z = es.origin_z + pz;

        // Direction + velocity
        f32 dx = bp.x_direction.sample_random(t);
        f32 dy = bp.y_direction.sample_random(t);
        f32 dz = bp.z_direction.sample_random(t);
        f32 vel = bp.velocity.sample_random(t);

        // Normalize direction
        f32 len = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (len > 0.001f) {
            dx /= len; dy /= len; dz /= len;
        } else {
            dy = 1.0f; // default upward
        }
        p.vel_x = dx * vel;
        p.vel_y = dy * vel;
        p.vel_z = dz * vel;

        // Acceleration
        p.accel_x = bp.x_accel.sample_random(t);
        p.accel_y = bp.y_accel.sample_random(t);
        p.accel_z = bp.z_accel.sample_random(t);
        if (bp.gravity) {
            p.accel_y -= 9.81f;
        }

        // Size
        p.size_start = bp.start_size.sample_random(t);
        p.size_end = bp.end_size.sample_random(t);

        // Rotation
        p.rotation = bp.initial_rotation.sample_random(t);
        p.rotation_rate = bp.rotation_rate.sample_random(t);

        // Lifetime
        f32 lt = bp.lifetime_curve.sample_random(t);
        p.lifetime = (lt > 0) ? lt : bp.lifetime;

        // Texture
        p.frame_rate = bp.frame_rate.sample_random(t);

        es.particles.push_back(p);
    }
}

// ---------------------------------------------------------------------------
// step_particles -- advance physics and age
// ---------------------------------------------------------------------------

void ParticleSystem::step_particles(EmitterState& es, f32 dt) {
    for (auto& p : es.particles) {
        // Integrate velocity
        p.vel_x += p.accel_x * dt;
        p.vel_y += p.accel_y * dt;
        p.vel_z += p.accel_z * dt;

        // Integrate position
        p.pos_x += p.vel_x * dt;
        p.pos_y += p.vel_y * dt;
        p.pos_z += p.vel_z * dt;

        // Rotation
        p.rotation += p.rotation_rate * dt;

        // Age
        p.age += dt;
    }

    // Remove expired particles
    es.particles.erase(
        std::remove_if(es.particles.begin(), es.particles.end(),
                       [](const Particle& p) { return p.age >= p.lifetime; }),
        es.particles.end());
}

// ---------------------------------------------------------------------------
// update
// ---------------------------------------------------------------------------

void ParticleSystem::update(f32 dt) {
    for (auto& es : emitters_) {
        if (es.active) {
            es.emitter_time += dt;

            // Check emitter lifetime expiry (repeating emitters loop)
            if (es.blueprint && es.emitter_time >= es.blueprint->lifetime) {
                if (es.blueprint->repeattime > 0) {
                    es.emitter_time = std::fmod(es.emitter_time,
                                                es.blueprint->lifetime);
                } else {
                    es.active = false;
                }
            }

            emit_particles(es, dt);
        }

        step_particles(es, dt);
    }

    // Prune emitters that are done and have no particles
    emitters_.erase(
        std::remove_if(emitters_.begin(), emitters_.end(),
                       [](const EmitterState& es) {
                           return !es.active && es.particles.empty();
                       }),
        emitters_.end());
}

// ---------------------------------------------------------------------------
// build_instances
// ---------------------------------------------------------------------------

const std::vector<ParticleInstance>&
ParticleSystem::build_instances(f32 /*cam_x*/, f32 /*cam_y*/, f32 /*cam_z*/) {
    instances_.clear();
    instances_.reserve(particle_count());

    for (const auto& es : emitters_) {
        if (!es.blueprint) continue;
        const auto& bp = *es.blueprint;

        u32 frame_count = bp.texture_frame_count;
        u32 strip_count = bp.texture_strip_count;
        f32 frame_w = (frame_count > 0) ? (1.0f / static_cast<f32>(frame_count)) : 1.0f;
        f32 frame_h = (strip_count > 0) ? (1.0f / static_cast<f32>(strip_count)) : 1.0f;

        for (const auto& p : es.particles) {
            ParticleInstance inst;
            inst.pos_x = p.pos_x;
            inst.pos_y = p.pos_y;
            inst.pos_z = p.pos_z;

            // Interpolate size over particle lifetime
            f32 life_frac = (p.lifetime > 0) ? (p.age / p.lifetime) : 0.0f;
            life_frac = std::clamp(life_frac, 0.0f, 1.0f);

            // Size curve if available, else lerp start->end
            f32 size_scale = 1.0f;
            if (!bp.size_curve.keys.empty()) {
                size_scale = bp.size_curve.sample(life_frac);
            }
            f32 base_size = p.size_start + (p.size_end - p.size_start) * life_frac;
            inst.size = base_size * size_scale;
            if (inst.size <= 0) inst.size = 0.1f;

            inst.rotation = p.rotation;

            // Fade out in last 20% of life
            f32 alpha = 1.0f;
            if (life_frac > 0.8f) {
                alpha = (1.0f - life_frac) / 0.2f;
            }
            inst.alpha = std::clamp(alpha, 0.0f, 1.0f);

            // Texture frame animation
            u32 frame = p.texture_frame;
            if (p.frame_rate > 0 && frame_count > 1) {
                frame = static_cast<u32>(p.age * p.frame_rate) % frame_count;
            }
            u32 strip = p.ramp_frame % std::max(strip_count, 1u);
            inst.uv_x = static_cast<f32>(frame) * frame_w;
            inst.uv_y = static_cast<f32>(strip) * frame_h;
            inst.uv_w = frame_w;
            inst.uv_h = frame_h;

            // Default white tint
            inst.r = 1.0f;
            inst.g = 1.0f;
            inst.b = 1.0f;

            instances_.push_back(inst);
        }
    }

    return instances_;
}

// ---------------------------------------------------------------------------
// particle_count
// ---------------------------------------------------------------------------

u32 ParticleSystem::particle_count() const {
    u32 total = 0;
    for (const auto& es : emitters_) {
        total += static_cast<u32>(es.particles.size());
    }
    return total;
}

} // namespace osc::renderer
