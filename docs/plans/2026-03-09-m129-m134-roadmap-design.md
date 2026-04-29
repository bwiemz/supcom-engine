# M129-M134 Roadmap — Design

## Overview

Six milestones covering cleanup, performance, and visual polish. Executed sequentially.

---

## M129: Particle System Polish

**Goal:** Commit the existing uncommitted particle system + sim_state changes.

**Scope:** Review the 4 modified files (emitter_blueprint.cpp, particle_system.cpp/hpp, sim_state.cpp), ensure they build and tests pass, commit.

**No design needed** — this is a cleanup commit.

---

## M130: Resolve Outstanding TODOs

**Goal:** Fix the 4 remaining TODOs in the codebase.

### 1. Maintenance consumption (army_brain.cpp:126)
Energy-gated shields/radar/stealth. When army energy efficiency < 1.0, maintenance-category consumers (shields, radar, intel) should reduce their effect proportionally. Implementation: add a `maintenance_efficiency` field to ArmyBrain economy, computed per-tick from energy availability. Shields/radar check this before operating.

### 2. Overlay heading (overlay_renderer.cpp:834)
Use entity heading (from quaternion) instead of hardcoded +Z for overlay direction indicators. Extract yaw from entity orientation quaternion.

### 3. Projectile VelocityAlign (weapon.cpp:168)
Read `Physics.VelocityAlign` from projectile blueprint Lua table. If present and true, orient projectile along velocity vector. Already partially implemented — just needs the blueprint field read.

### 4. Being-captured state (moho_bindings.cpp:942)
Track `being_captured_` bool on Unit. Set true when capture begins, false when capture completes or is interrupted. Used by `IsBeingCaptured()` Lua query.

---

## M131: Frustum Culling

**Goal:** Cull entities outside the camera frustum before rendering, reducing GPU draw calls and CPU instance collection work.

### Architecture

New `Frustum` class extracts 6 clip planes from the VP matrix using Gribb-Hartmann method. Provides `is_sphere_visible(center, radius) -> bool`.

Each frame, renderer constructs a Frustum and passes it to sub-renderers. Sub-renderers call `is_sphere_visible()` per entity, replacing ad-hoc distance checks.

### Frustum Plane Extraction (Gribb-Hartmann)

From column-major VP matrix `m[16]`:
```
left:   (m[3]+m[0], m[7]+m[4], m[11]+m[8],  m[15]+m[12])
right:  (m[3]-m[0], m[7]-m[4], m[11]-m[8],  m[15]-m[12])
bottom: (m[3]+m[1], m[7]+m[5], m[11]+m[9],  m[15]+m[13])
top:    (m[3]-m[1], m[7]-m[5], m[11]-m[9],  m[15]-m[13])
near:   (m[3]+m[2], m[7]+m[6], m[11]+m[10], m[15]+m[14])
far:    (m[3]-m[2], m[7]-m[6], m[11]-m[10], m[15]-m[14])
```
Normalize each plane. Sphere visible when `dot(normal, center) + d > -radius` for all 6 planes.

### Bounding Radius Source

- **Units/Props**: GPUMesh::uniform_scale from MeshCache. Fallback: footprint * 0.5 for cube-rendered.
- **Projectiles**: Fixed 2.0 units.
- **Particles**: Per-emitter max particle size.
- **Decals**: Per-decal scale from StoredDecal.

### Integration

| Renderer | Current | New |
|----------|---------|-----|
| UnitRenderer | Props: 600u distance | All: frustum sphere test |
| OverlayRenderer | 800u distance | Frustum sphere test |
| StrategicIconRenderer | 1200u + screen bounds | Frustum pre-filter + keep screen bounds |
| ParticleSystem | None | Frustum test per emitter |
| Decals | None | Frustum test per decal |

### Files

| File | Change |
|------|--------|
| `src/renderer/frustum.hpp/cpp` | New: Frustum class |
| `src/renderer/CMakeLists.txt` | Add frustum.cpp |
| `src/renderer/renderer.cpp` | Construct Frustum, pass to sub-renderers |
| `src/renderer/unit_renderer.hpp/cpp` | Accept Frustum, replace distance cull |
| `src/renderer/overlay_renderer.hpp/cpp` | Accept Frustum |
| `src/renderer/particle_system.hpp/cpp` | Accept Frustum |
| `tests/test_frustum.cpp` | Plane extraction + sphere visibility tests |

---

## M132: LOD System

**Goal:** Switch mesh quality based on camera distance using FA's blueprint LOD cutoff values.

### Architecture

FA blueprints define LODs[1] through LODs[n] with LODCutoff distances. MeshCache already loads the primary mesh. Extension: for each blueprint, resolve multiple mesh paths (one per LOD level). At collection time, UnitRenderer picks the LOD whose cutoff encompasses the entity's distance from camera.

### Scope

- Read LODCutoff values from blueprint Lua tables during mesh resolution
- Store per-blueprint LOD mesh list in MeshCache
- UnitRenderer selects LOD at instance collection time based on camera distance
- Fallback: if only 1 LOD loaded, use it at all distances (current behavior)

---

## M133: Unit Death Animations

**Goal:** Play SCA death animation when units die, then swap to wreckage mesh.

### Architecture

Currently units are destroyed immediately on death. Change: on death, enter a "dying" state that plays the death animation SCA for its duration, then transitions to wreckage (or removal).

- Unit gets `dying_` bool + `death_timer_` float
- On kill: set dying_=true, play death SCA via AnimManipulator (uses M127 blending)
- Each tick: decrement death_timer_; when 0, destroy unit and optionally spawn wreckage prop
- Renderer: dying units render normally (animation plays out)
- Dying units are removed from targeting, command processing, collision

### Files

| File | Change |
|------|--------|
| `src/sim/unit.hpp/cpp` | Add dying state, death timer, death animation trigger |
| `src/sim/entity.hpp` | Add `is_dying()` query |
| `src/renderer/unit_renderer.cpp` | Continue rendering dying units |
| `tests/test_death_anim.cpp` | Verify death state transitions |

---

## M134: Bloom Post-Processing

**Goal:** Add bloom effect for energy weapons, shields, explosions.

### Architecture

Two-pass post-processing:
1. Render scene to offscreen HDR framebuffer (R16G16B16A16_SFLOAT)
2. Brightness extraction pass: threshold fragments above 1.0 intensity
3. Gaussian blur (2-pass separable: horizontal then vertical, at half resolution)
4. Composite: additive blend bloom texture onto scene, then present

### Scope

- New offscreen framebuffer + resolve to swapchain
- Brightness threshold shader
- Separable Gaussian blur shader (half-res for performance)
- Composite shader (scene + bloom)
- Adjustable threshold and intensity uniforms

This is the most complex milestone in the batch — may warrant its own detailed design session when we get to it.
