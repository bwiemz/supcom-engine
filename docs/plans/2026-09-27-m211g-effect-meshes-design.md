# M211g: the build effects' meshes, Moho's shader names, and burnt trees

M211f drew units under construction with FA's build shaders, but a UEF
structure under construction still sat inside an opaque box: the build cube
Lua raises around it is a projectile whose mesh names `UEFBuildCube`, which
the engine didn't know. Its slices (`AlphaFade`), its first flash
(`TMeshGlow`) and Aeon's pool (`AeonBuildPuddle`) were the same. Burnt trees
drew flat black: M211d sent `BlackenedNormalMappedAlpha` down the wreck
path, but it is its own technique. Retail `mesh.fx` is read for the formulas,
never copied; faf-re's `ShaderDictionary` and `UserEntity` for the rest.

## Shader names

Moho resolves a LOD's `ShaderName` through a dictionary of legacy names
(`ResolveShaderAnnotationName`): `TMeshNoLighting` → `Flat`, `TMeshNoNormals`
→ `VertexNormal`, `TMeshAlpha` → `NormalMappedAlpha`, `TMeshGlow` →
`NormalMappedGlow`, `TMeshTerrain` → `NormalMappedTerrain`, `Simple` and
`Team` → `Unit`, `TMeshAlphaGlowFade` → `UnitBuild`, `TMeshMetalBuild` →
`AeonBuild`, `TMeshShield` → `Shield`, `TMeshZFill` → `ShieldFill`,
`TMeshAdd` → `Effect`, `TMeshExplosion` → `Explosion`, `TMeshCloud` →
`Cloud`, `TMeshOuterCloud` → `OuterCloud`, `TMeshEMPNuke` → `NukeEMP`,
`TMeshQuantumNuke` → `NukeQuantum`, `TMeshTemporalBubble` →
`TemporalBubble`. Other names pass through; an empty one is `Unit`. The
engine's `mesh_technique` applies it first; what it names and the engine
hasn't ported still draws as `Unit`.

## The instance's colour

`UserEntity::CreateMeshInstance` gives a mesh instance colour `-1`, white;
only `UserUnit`'s is the army's. Projectiles took their army's colour in the
engine; they take white. (Props already did.)

## The techniques

| Technique | Pixel shader | Render state | Shadow |
|---|---|---|---|
| `NormalMappedAlpha` | `NormalMappedPS(maskAlbedo false, glow false)`: the albedo times the instance's colour, alpha `f * albedo.a`, tested over `0x80` | opaque, colour only | as the unit |
| `NormalMappedGlow` | `NormalMappedPS(false, true)`: the albedo times the instance's colour, alpha the glow | opaque, colour and alpha | as the unit |
| `BlackenedNormalMappedAlpha` | as `NormalMappedAlpha`, the albedo first greyed to `dot(rgb, 0.1)`, its highlight `pow(., 8) * spec.g` | opaque, colour only | as the unit |
| `AlphaFade` | `AlphaFadePS(2.0, 0.145)`: the albedo lit by the vertex normal, alpha `albedo.a * f * sat(1 - (age - 2) * 0.145)`, tested over `0x23` | blended, colour and alpha | none (no depth stage) |
| `UEFBuildCube` | no light: albedo at `(uv + age (0.012, 0.062)) * 0.025`, secondary at `(uv + age (0.012, 0.124)) * 50`; `lerp(lerp(albedo + secondary, blue, max(frac(0.05 time), 0.35)), albedo, f)`, alpha `max(f, 0.5) * albedo.a` | blended, colour only, **no depth write** | as the unit |
| `AeonBuildPuddle` | `AeonBuildPS`'s light and terms, every read at `uv + age (-0.002, 0.0042)`, no team colour, alpha the glow | opaque, colour and alpha | as the unit |

`f` is the fraction complete (the entity's; 1 for projectiles), `age` the
instance's age in ticks. The engine keeps its prop rule: FA's colour-only
techniques leave the frame's alpha, which the engine writes as 0 (at most
the terrain's 0.01 plus specular beneath).

## The engine

- `mesh_technique` resolves the dictionary, then maps the six new names.
- `is_wreckage_shader` drops `BlackenedNormalMappedAlpha`.
- The unit renderer gives non-unit instances white; `AlphaFade` and
  `UEFBuildCube` groups draw with the blended ones.
- A fourth mesh pipeline: blended, colour only, depth not written
  (`UEFBuildCube`). `AlphaFade` takes the overlay pipeline (colour and alpha
  blended).
- The shadow pass skips `AlphaFade`.

## Tests

`--effect-mesh-test` (gate). Real units for the wiring, plates of the test's
own (over the sky's clear colour, a white fill) for the formulas:

1. **The build effects.** A UEF engineer and an Aeon one build: the cube,
   its slices and the pool are projectiles drawn by `UEFBuildCube`,
   `AlphaFade` and `AeonBuildPuddle`, and in white.
2. **`UEFBuildCube`.** Built, its albedo over the sky at its alpha; at 0, the
   pulse toward blue at 0.9 (unclamped above) and held at 0.35 below.
3. **`AlphaFade`.** Its albedo just made, at 0.42 five ticks on, gone ten on;
   30% built at 0.3, 10% under its alpha test.
4. **Unmasked.** `TMeshGlow` and `TMeshAlpha` (resolved) tint a unit's white
   albedo by its army's colour, where `Unit` leaves it white; `TMeshAlpha`
   40% built fails its test.
5. **`AeonBuildPuddle`.** A striped albedo moves 0.1 of the plate over 50
   ticks; the same plate drawn by `Unit` doesn't.
6. **Burnt trees.** A prop drawn by `BlackenedNormalMappedAlpha`: 0.3 grey
   from a white albedo; 40% built, cut by its test.

`--bloom-test` gains a sixth check: a build slice just made glows fully, its
alpha blended as well as its colour.

Unit tests cover the dictionary. Not checked: `UEFBuildCube` leaving depth
unwritten (nothing in the test's scene draws behind it afterwards), and
`AlphaFade`'s missing shadow.

Two things about the plates, learnt here: a structure spawned whole runs
retail's `StopBeingBuiltEffects`, which puts its mesh back (Aeon's two
seconds on) and starts a new mesh instance's age; and sensors, storages, T3
generators and fabricators animate the bone a plate hangs on. The stand-ins
are now structures that kept a plate still over 100 ticks.

## Left for later

- The props' other techniques: `VertexNormal`, `NormalMappedTerrain`,
  `UndulatingNormalMappedAlpha` (313 of retail's blueprints between them).
- Shields' techniques, `Effect`, `Explosion`, the nukes' and clouds'.
- `DepthClip`: alpha-tested meshes' shadows are cut by their alpha in FA.
- What burns trees near SCMP_009's first base a few seconds in.
- Hidden bones (M211h).
