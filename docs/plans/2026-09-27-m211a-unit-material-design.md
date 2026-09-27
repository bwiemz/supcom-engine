# M211a: meshes shade as FA's NormalMappedPS

M210a gave meshes FA's light (`ComputeLight`), but not FA's material: the
engine kept its own Blinn-Phong highlight on top. FA's standard mesh shader
is `NormalMappedPS` in retail `mesh.fx` (read for its formulas, never
copied). The `Unit` technique uses it with the team colour masked in, and
props' `NormalMappedAlpha` without the mask:

```
albedo   = lerp(albedo, teamColour, specular.a)      (Unit; props: albedo * colour)
env      = texCUBE(environment, reflect(-V, N))       V: from the eye to the point
highlight= (0.6, 0.8, 0.9) * saturate(reflect(S, N) . -V)^2 * specular.g
colour   = albedo * (2 * specular.b + light + 2 * env * specular.r) + highlight
```

The constants (`NormalMappedPhongCoeff`, `glowMultiplier` 2) are set in
`mesh.fx` itself. The SpecTeam texture's channels are: red, how much the
environment is reflected; green, the highlight; blue, glow; alpha, the
team colour's mask.

**Two things about FA's formula.**
- It reflects the direction *to* the eye, `-V`, not the view ray. Off a
  wall facing the camera that points up, so units show the environment
  cube's sky rather than its ground.
- The highlight doesn't depend on the sun's colour, only its direction.

## The environment cube

Moho binds, for each material, the map's environment cube named by the
technique's `environment` annotation, with the map's `<default>` as the
fallback (`CWldTerrainRes::GetEnvLookup`, faf-re). Without one, its own
default is `/textures/environment/defaultenvcube.dds`. `Unit` names none,
so it takes `<default>`. SCMP_009's is `envcube_evergreen01a.dds`: 512²
DXT1, six faces.

## The engine

- **The DDS parser reads cubemaps**: `DDSCAPS2_CUBEMAP` with all six
  faces, stored face by face, each with its mips. A cube missing a face,
  or cut short, is refused.
- **The texture cache uploads them** as cube-compatible six-layer images
  with cube views (`get_cube_blocking`), and has a black 1×1 cube for a
  scene without one. A cubemap fetched as a 2D texture is refused.
- **The light set** (which the lit shaders already bind) gains binding 2,
  the environment cube, written when a scene is built.
- **The mesh shader** is FA's `NormalMappedPS`. Props are flagged by a
  negative green in their instance colour, and skip the team mask. Wrecks
  keep the engine's burnt look until `WreckagePS`, which has its own
  crunch texture, is ported (M211b).
- The faction techniques (`Aeon`, `Insect`, `Seraphim`, `Metal`...) all
  draw as `Unit` for now; they are M211b's.

## Tests

- **Unit** (`[dds]`): a cube parses as six faces, face by face, each with
  its mips; a 2D DDS is one face; a partial or cut-short cube is refused.
- **`--material-test`** (gate): a UEF T1 land factory on flat ground of
  the test's own, under a white fill and no sun (so its light is 1).
  1. The map's cube brightens it (10,098 pixels) against the black cube,
     and darkens nothing.
  2. A red army and a green army differ only where the team mask is set
     (1,709 pixels, redder and greener, blue unchanged), which is under
     half the factory.
  3. With no light, a black cube and no sun, only glow shows: a T3 power
     generator's core (762 pixels).
  4. The highlight moves with the sun's direction alone (590 pixels).
  5. A cube lit above lights the factory more than one lit below, as FA
     reflects `-V`.
