# M211d: wrecks shade as FA's WreckagePS

The engine drew wrecks with a stand-in: the albedo greyed and darkened,
with a faint highlight. FA's `Wreckage` technique is `WreckageVS` and
`WreckagePS` in retail `mesh.fx` (read for its formulas, never copied).

A wreck's mesh blueprint is its unit's, copied by retail's
`ExtractWreckageBlueprint` (`/lua/system/Blueprints.lua`). The copy sets
each LOD's `ShaderName` to `Wreckage` and its `SpecularName` to
`/env/common/props/wreckage_noise.dds`: the "specular" texture of a wreck
is a crunch noise.

```
crunch = tex2D(specular, (uv + (o, -o)) * 5.15)       o = frac(0.01 * material.x)
colour = albedo * ComputeLight(N . S, 1)                no shadow
colour *= crunch.g < 0.22 ? (albedo + crunch.r + crunch.a) * crunch.b * 2.5
                          : crunch.b * 2
```

FA's comment on the missing shadow: "the random crunchiness makes for bad
artifacts".

## The instance's time

`material.x` is the mesh instance's time: the game tick it was made, modulo
36000 (faf-re's `HardwareMeshBatch`, `kMeshShaderTimeWrapSeconds`; the
per-instance `material` is that time, the technique's parameter and the UV
scroll). Moho makes a mesh instance when an entity appears or changes mesh.
The unit renderer does the same:
- It records, per entity, the tick it first drew it with its current
  blueprint or mesh override, whether or not it was in view.
- It forgets the record when the entity is gone.
- `MeshInstance` carries the time (88 bytes, attribute 13).

## The crumple

`WreckageVS` moves each vertex, in the world:

```
n = normalize(p), s = n.x * 0.15, r = |p|, phi = frac(0.01 * |instance position|)
p.x += sin(14.5 r n.z + phi) s
p.y += cos(10.8 r n.x + phi) s
p.z += sin(20.5 r n.y + phi) s
```

(FA writes `float s = nvert * 0.15`, which keeps x.) The shadow pass
doesn't crumple, as FA's `Depth` technique doesn't.

## Tests

`--material-test` (gate). The plates stand off the test's ground.

13. **The two branches.** Wrecks with a uniform crunch
    `(0.2, g, 0.2, 0)`, lit by the fill (1):
    - a white albedo shows 153 (0.6) with g = 0.2;
    - a grey one shows 45 (0.5 × 0.7 × 0.5);
    - a white one with g = 0.25 shows 102 (0.4).
14. **Tiling and offset.** A crunch in stripes, half under 0.22 and half
    over, crosses an 8-unit plate's middle row 8 times in 160 pixels (5.15
    repeats). Two wrecks made 6 ticks apart differ on 106 of those pixels.
15. **The crumple.** A finely cut red plate's edge strays from a line by
    0.5 pixels as a unit, and by 6.8 as a wreck.
16. **No shadow.** A plate hovering 2.5 over another, under a sun
    overhead, darkens a unit's plate (2,548 pixels) but not a wreck's (0).

**Mutation testing: 9 of 9 mutants killed.**
- The crumple: none; by y.
- The crunch: untiled; no offset; births never renewed.
- The branches: at 0.5; no albedo in the factor; ×2.5 over the threshold.
- A wreck taking shadows.

## Left for later

- **`AeonCZAR`**: the CZAR's own shader.
- The build shaders.
- Glow into bloom (M211e).
