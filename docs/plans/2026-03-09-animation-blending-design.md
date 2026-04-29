# M127: Animation Blending & Transitions

## Problem

AnimManipulator snaps instantly between animations (e.g., idle→walk→attack). Units appear robotic. Multiple AnimManipulators targeting the same unit use last-write-wins semantics, which is mostly correct but lacks a formal identity reset.

## Solution

Add cross-fade transitions to AnimManipulator and formalize multi-animator bone composition.

### Cross-Fade Mechanism

When `PlayAnim()` is called while an animation is already playing:

1. Snapshot current `animated_bone_matrices_` into `blend_from_matrices_[]`
2. Set `blend_remaining_ = blend_time_` (default 0.2s)
3. Start new animation at frame 0

Each tick during blend:
- Compute "to" bone matrices from the new animation normally
- `weight = blend_remaining_ / blend_time_`
- For each owned bone: `matrix = lerp(to_matrix, from_matrix, weight)`
- `blend_remaining_ -= dt`, clamp to 0

Matrix lerp is component-wise 4x4 linear interpolation — not rotation-correct like slerp, but imperceptible at short durations (0.2-0.3s) and much cheaper.

### Multi-Animator Composition

Each tick before manipulators run, `animated_bone_matrices_` is reset to identity. Each AnimManipulator writes only bones it owns (respecting `disabled_bones_`). RotateManipulator/AimManipulator/SlideManipulator overwrite their bones on top. No layer system or animation state machine — FA scripts manage animation selection via Lua threads.

### Lua API

```lua
-- Existing (unchanged):
animator:PlayAnim('/units/uel0001/uel0001_awalk.sca', true)
animator:SetRate(1.0)

-- New:
animator:SetBlendTime(0.3)  -- seconds, default 0.2
```

## Files Changed

| File | Change |
|------|--------|
| `src/sim/manipulator.hpp` | Add `blend_from_matrices_`, `blend_time_`, `blend_remaining_` to AnimManipulator |
| `src/sim/manipulator.cpp` | Snapshot on PlayAnim, blend logic in `compute_bone_matrices()` |
| `src/sim/unit.cpp` | Reset `animated_bone_matrices_` to identity before `tick_manipulators()` |
| `src/lua/moho_bindings.cpp` | Add `SetBlendTime` to AnimationManipulator method table |

No renderer changes. No shader changes. No new files.

## Data Flow

```
Unit::tick()
  → reset animated_bone_matrices_ to identity
  → tick_manipulators(dt, L)
    → AnimManipulator::tick(dt)
      → advance animation time
      → compute_bone_matrices()
        → interpolate SCA frames → "to" matrices
        → if blending: lerp(to, from, weight) per bone
        → write to unit->animated_bone_matrices_[scm_idx]
    → RotateManipulator/AimManipulator/SlideManipulator write their bones
  → Renderer reads animated_bone_matrices_ → SSBO upload (unchanged)
```

## Design Constraints

- Default blend time 0.2s — short enough to feel responsive, long enough to smooth transitions
- Backward compatible: existing FA scripts work without changes (automatic cross-fade)
- No new dependencies, no renderer/GPU changes
- Memory cost: one extra `vector<array<f32,16>>` per AnimManipulator during active blend (~4KB for 64 bones, transient)
