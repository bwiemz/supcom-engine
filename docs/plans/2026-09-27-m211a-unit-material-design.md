# M211a: meshes shade as FA's NormalMappedPS

M210a gave meshes FA's light (`ComputeLight`), but not FA's material: the
engine kept its own Blinn-Phong highlight on top. FA's standard mesh shader
is `NormalMappedPS` in retail `mesh.fx` (read for its formulas, never
copied). The `Unit` technique uses it with the team colour masked in, and
props' `NormalMappedAlpha` without the mask:

```
albedo   = lerp(albedo, teamColour, specular.a)      (Unit; props: albedo * colour)
env      = texCUBE(environment, reflect(-V, N))       V: toward the eye (below)
highlight= (0.6, 0.8, 0.9) * saturate(reflect(S, N) . -V)^2 * specular.g
colour   = albedo * (2 * specular.b + light + 2 * env * specular.r) + highlight
```

The constants (`NormalMappedPhongCoeff`, `glowMultiplier` 2) are set in
`mesh.fx` itself. The SpecTeam texture's channels are: red, how much the
environment is reflected; green, the highlight; blue, glow; alpha, the
team colour's mask.

**FA's view direction.** `V` is not `normalize(eye - point)`. The vertex
shader takes the point's normalised device position, `clip.xyz / clip.w`,
and turns it into the world by the view's rotation. Moho's camera is
right-handed (faf-re: `CameraImpl::UpdateCoords` puts the eye along the
view's +Z, and `VEC_D3DProjectionMatrixFOV` sets w = -z), so that depth,
near 1, runs back toward the eye. `V` therefore points to the eye along
the view axis, but its sideways part is mirrored (and steepened by the
frustum's slope, 1 / tan of the half angle). At the middle of the screen
it is the direction to the eye. Right of the middle, it leans right where
the direction to the eye leans left.

The rest of the formula is textbook with that `V`:
- `reflect(-V, N)` reflects the view ray. Off a wall facing the camera, it
  points down, so walls show the environment cube's ground.
- The highlight is Phong's, with the reflected light, `-reflect(S, N)`,
  against the direction to the eye. It follows the sun's direction but not
  its colour, and nothing masks it by `N . S`: a sun under the ground still
  lights walls facing the camera.

The engine rebuilds FA's `V` per pixel from `viewProj`: the rows hold the
view's axes (right, up, and back, negated where the device's y runs down).

## The normal

`N` is FA's `ComputeNormal`: the normal map's green runs along the
vertex's binormal, its alpha along the tangent (DXT5nm's y and x), and z
along the normal:

```
N = normalize(g * binormal + a * tangent + z * normal)
```

An SCM vertex stores the normal, then the tangent, then the binormal
(faf-re's `SScmVertex`). The files agree: over 22,348 triangles of 40
retail meshes, the second vector lies along the face's normal on 91%, the
third along the direction u grows on 96%, the fourth along v on 93%. The
engine's parser had read the first two the other way round since before
M211a, so every mesh shaded with its tangent for its normal. It now reads
all three; the vertex carries the binormal (76 bytes).

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
- **`--blend-test`**: of the UEF ACU's 3,382 triangles, the parsed normal
  lies along the face on 3,380, the tangent along u on 3,380, the
  binormal along v on 3,369.
- **`--material-test`** (gate): a UEF T1 land factory on flat ground of
  the test's own, under a white fill and no sun (so its light is 1).
  1. The map's cube brightens it (9,239 pixels) against the black cube,
     and darkens nothing.
  2. A red army and a green army differ only where the team mask is set
     (1,707 pixels, redder and greener, blue unchanged), which is under
     half the factory.
  3. The highlight: a sun overhead lights the roofs (8,579 pixels); one
     under the ground still lights the walls facing the camera (3,669),
     as FA doesn't mask the highlight by `N · S`.
  4. A cube lit above lights the factory more than one lit below (1,440
     pixels, against 760): the roofs reflect the sky.
  5. FA's `V` is mirrored sideways. The test's own mirror (a flat plate,
     white, SpecTeam red 1, drawn as the UEF wall's mesh) reflects the
     world's left when it stands right of the view, and its right when it
     stands left (34,450 pixels each; none the other way). The direction
     to the eye would reflect the other side.
  6. With no light, a black cube and no sun, only glow shows: a T3 power
     generator's core (762 pixels).
  9. A plate whose normal map leans by its alpha reflects the world's +X,
     along its tangent (34,448 pixels; none the other way).
  10. One leaning by its green reflects the world's +Z, along the
      binormal it stores, not `cross(N, T)`, which is turned over there
      (34,448 pixels; none the other way).
