# M214d: FA's refracting particles

## Why

M214c drew four of particle.fx's five TRamp blends and skipped the fifth,
REFRACT (`BlendMode = 5`). 21 of retail's 2,724 emitters use it, for heat
and distortion effects:

- the shimmer over a hydrocarbon plant;
- the heat haze of a burning, damaged structure;
- distortion rings;
- the heavy-proton and disintegrator hits;
- the nuke's launch trail.

The engine drew none of them.

The rules come from:

- particle.fx's `WorldRefractPS` and its `T*_REFRACT` techniques;
- faf-re's `CWorldParticles::AddWorldParticle` and `RenderRefractingEffects`;
- faf-re's `WRenViewport::RenderRefractingEffects`, `RenderCopyForRefraction`
  and `Render`'s pass order.

Retail's files are read for the rules, never copied.

## The rules

- **Their own lane.** `AddWorldParticle` puts every particle whose blend
  mode is 5 into the refracting buckets, whatever its SortOrder. The water's
  side and the draw order of the other particles don't apply to it.
- **Drawn last** (`WRenViewport::Render`). The world is drawn first:
  - the meshes and effects before and after the water;
  - the water;
  - the post-water meshes;
  - the fog of war.

  Then `RenderRefractingEffects` copies the frame into the refraction sheet
  (`RenderCopyForRefraction`, the viewport's rectangle) and draws the
  refracting buckets reading it. A refracting particle bends everything
  drawn before it, the water included.
- **The pixel** (`WorldRefractPS`):
  - `offset = 0.005·(2·texel.rg − 1)`, where `texel` is the particle's
    texture, sampled as the other blends sample it;
  - `colour = background(screen + offset).rgb`;
  - `alpha = texel.a · ramp(age, selection).a`.

  `screen` is `WorldVS`'s: the vertex's projected position, `0.5·xy/w + 0.5`,
  interpolated across the quad. The background's sampler is linear and
  clamped.
- **The state** (`TRamp_REFRACT` and its kin): SrcAlpha / InvSrcAlpha, RGB
  only (the frame's alpha, its glow, stands). Depth tested less, not
  written, no culling. The quads are the other blends' quads, with the same
  billboards, flat quads, alignment and animation.

## The engine

- **`ParticleSystem`.** It draws the blend-5 particles too, in a lane after
  all the others. Its groups carry the blend, and it says whether any are
  drawn this frame.
- **`ParticleRenderer`.**
  - `render` skips blend 5, as before.
  - `render_refracting` draws only blend 5, with a pipeline of its own:
    `particle_refract_frag`, and the background as a third set.
  - `particle_vert` also passes the screen position.
  - `set_background` points the third set at the renderer's copy of the
    frame.
- **`Renderer`.**
  - On a frame with refracting particles, the scene pass ends with its
    colour still an attachment. The frame is copied into the refraction
    image (`copy_refraction`, as the water's copy is), and a last pass
    loads the frame and draws them.
  - On a map with water, the passes run: first, copy, middle (the water on),
    copy, last. The middle pass is new: it loads, and keeps its colour an
    attachment.
  - A frame without refracting particles keeps the passes it had.

## Tests

`--refract-render-test` (gate). It uses a plate of two colours, split by a
line down the frame's middle (the spot's trees are cleared, as they sway),
and refracting billboards over the line. Billboards keep every corner at one
depth, so their interpolated screen position is exact.

1. **The offset.** The texture's red is 1 and its green a half, so its
   offset is 0.005 of the frame across (4 pixels at 800). Within the
   particle, the frame shows the plate as it lies 4 pixels to the right,
   within 0.0001.
2. **The alpha.** With texture alpha 0.5 and ramp alpha 0.5, each particle
   is a quarter the displaced frame over what's beneath. The emitter's still
   particles lie over one another, n deep: 1 − 0.75ⁿ.
3. **Its own lane.** An emitter with a negative SortOrder (which puts other
   particles under the water) refracts the same.
4. **Drawn last.** Over open water whose waves are held still, the particle
   shows the water's surface displaced: the finished frame.

## Left for later

- `SetEmitterCurveParam` and the other emitter parameter bindings.
- The fidelity options (`graphics_Fidelity`: FA copies for refraction only
  at medium fidelity and above).
