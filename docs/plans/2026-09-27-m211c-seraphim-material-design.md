# M211c: Seraphim's falloff, and FA's alpha rules

M211b drew Seraphim meshes as `Unit`. FA's `Seraphim` technique is
`UnitFalloffPS` in retail `mesh.fx` (read for its formulas, never copied):

```
N.V      = pow(1 - saturate(dot(normalize(V), N)), 0.6)
falloff  = tex2D(lookup, (N.V, colorLookup))           point-sampled, clamped
env      = texCUBE(<seraphim>, reflect(-V, N)) * specular.r * falloff.a
phong    = (0.5, 0.6, 0.7) * saturate(reflect(S, N) . -V)^9 * specular.g
light    = sunAmbient + (1 - sunAmbient) * shadowFill   (shadow = 0: no sun)
colour   = albedo.rgb * light + env + phong + falloff.rgb * albedo.a
```

Three things set it apart from the other techniques:
- **No sun.** FA leaves its shadow term at 0 (`ComputeShadow` is commented
  out), so a Seraphim mesh takes only the ambience and the shadow fill,
  without the lighting multiplier.
- **No team tint on the albedo.** The army's colour comes through the
  lookup instead: its row.
- **The albedo's alpha is a mask**, of the falloff's colour: on a
  Seraphim ACU, 84% of the albedo's texels have alpha under 0.1.

## The lookup

`lookupTexture` is per material: the mesh LOD's `LookupName` (faf-re's
`MeshMaterial::Create`). 179 retail blueprints name
`/textures/environment/Falloff_seraphim_lookup.dds` (512², uncompressed);
two UEF shield meshes name a phase-shield lookup. The sampler is point,
clamped, without mips; the engine reads it with `texelFetch`, which is that
whatever the texture's sampler.

Its row is the instance's `colorLookup` (`UserUnit::CreateMeshInstance`):

```
colorLookup = (clamp(i, 0, n - 1) + 0.5) / n
```

`n` is `#GameColors.PlayerColors` (at least 1). `i` is the army colour's
index in `GameColors.ArmyColors` (`func_GetColorIndex`): 3 if it isn't
there, 0 for an entity without an army. In retail's lookup the first three
rows (red, dark green, blue) have glows of their own; from the fourth on
they share one.

## FA's alpha rules

The engine discarded any mesh pixel whose albedo alpha, times the
instance's, was under 0.1. So most of every Seraphim unit disappeared. FA
alpha-tests one technique, the props' `NormalMappedAlpha`: `albedo.a` (times
`material.g`, 1 here) must be over `0x80` (`AlphaFunc = Greater`). The unit
techniques write glow into alpha and never test it. The engine now does the
same: props discard at or under `128/255`, and units keep only the
instance's alpha (a unit under construction fades).

## The engine

- **`sim::read_game_colors`** reads `GameColors` (army and player colours)
  through the state's `import`. It moved out of `session_manager` (#138's
  `game_army_colors`), because the renderer needs it too and may not depend
  on the Lua layer.
- **`MeshInstance.color_lookup`** (84 bytes; attribute 12, passed on flat)
  comes from `team_color_lookup(ArmyRecord*, GameColors)`. The renderer
  reads the tables when it builds a scene.
- **The mesh cache** reads each LOD's `LookupName`, falling back to LOD 1's
  as the other textures do. A draw group binds it at set 5, or transparent
  black while it loads or if there is none.
- **The mesh shader** gains the `Seraphim` branch and FA's alpha rules.

## Tests

- **Unit** (`[renderer]`): an army's row is its colour's index; a colour
  not listed takes row 3 and no army row 0; the row is clamped below the
  player colours' count, and without tables there is one row.
- **`--army-colors-test`**: `GameColors` has ten player colours too.
- **`--material-test`** (gate):
  11. A unit plate whose albedo's alpha is clear still draws (32,258
      pixels). Prop plates at the alpha reference draw nothing; just over
      it, whole (34,448). The props are two the map has none of.
  12. A Seraphim plate, lit by nothing, shows its lookup's texel: 16 × 20,
      red counting columns and green rows. Checks:
      - column 4, as `pow(1 - sin(pitch), 0.6)` gives;
      - row 5 for an army coloured `ArmyColors[3]`, row 7 for a colour not
        listed;
      - unchanged under a white environment (the texel's alpha is 0) and
        under a sun overhead;
      - a black albedo under a full team mask, lit by the fill, stays black.

**Mutation testing: 11 of 11 mutants killed.**
- The old alpha rule; props tested at 0.1 or passing at the reference.
- The row without its half; default row 0.
- The falloff without its power, or with `V` turned.
- The environment not weighed by the falloff's alpha.
- The sun lighting it; the lookup filtered; the albedo tinted.

## Left for later

- **`AeonCZAR`** and **`WreckagePS`** (with its crunch texture).
- The build shaders.
- **Glow and bloom from alpha** (M214): FA writes `specular.b + glowMinimum`
  to alpha, which its bloom reads.
