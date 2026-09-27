# M211b: the factions' materials

M211a drew every mesh with FA's `Unit` technique (`NormalMappedPS`). A
unit blueprint's mesh names its technique in `LODs[n].ShaderName`, and
across retail's units:

| ShaderName | Units | FA pixel shader |
|---|---|---|
| `Unit` | 388 (UEF, and others) | `NormalMappedPS` |
| `Aeon` | 219 | `AeonPS` |
| `Seraphim` | 219 | `UnitFalloffPS` |
| `Insect` | 211 (Cybran) | `NormalMappedInsectPS` |
| `AeonCZAR` | 2 | `AeonCZARPS` |
| `Metal` | 1 | `NormalMappedMetalPS` |

M211b ports Aeon, Insect and Metal; Seraphim's falloff (a per-mesh ramp
texture) and the CZAR are M211c's, and draw as `Unit` until then.

## FA's formulas (retail `mesh.fx`, read for formulas only)

All mask the team's colour in by the SpecTeam alpha, add glow
(`2 · specular.b`), and reflect `R = reflect(-V, N)` as `Unit` does.

- **`AeonPS`** reflects the map's `<aeon>` cube (its technique's
  `environment` annotation), weighted by `specular.r` (not doubled). Its
  highlight is `(0.8, 0.85, 1.10) · (reflect(S, N) · -V)^3 · specular.g`
  (`AeonPhongCoeff`), and it takes the sun at 0.6: `light = 0.6 ·
  multiplier · L + (1 - L) · fill`.
- **`NormalMappedInsectPS`** (Cybran) reads its highlight from a lookup,
  `/textures/engine/insectlookup.dds`, at `(reflect(S, N) · -V, N · S)`,
  clamped. It adds half the environment (`0.5 · specular.r · env`), keeps
  the highlight off the team's colour (`· (1 - specular.a)`), and takes
  the sun twice over: `L = 2 · sun · (N · S) · shadow + ambience`.
- **`NormalMappedMetalPS`** is Insect's shape with
  `/textures/engine/anisotropiclookup.dds` and FA's ordinary light.

Moho binds the two lookups for every mesh (faf-re `Mesh.cpp`).

## The engine

- **The mesh cache** reads each LOD's `ShaderName` into a `MeshTechnique`.
- **The mesh push block** carries it per draw: groups are per mesh (88
  bytes, from 84).
- **The light set** gains the `<aeon>` and `<seraphim>` cubes (bindings 3
  and 4) and the two lookups (5 and 6). Each cube falls back to the map's
  `<default>`, as Moho's `GetEnvLookup` does. A clamped sampler reads the
  lookups.
- **The mesh shader** branches on the technique.

## Tests

`--material-test` (gate) adds:
7. UEF, Aeon, Cybran, Seraphim and Metal blueprints resolve to their
   techniques.
8. With a white `<aeon>` cube and a black `<default>`, an Aeon land
   factory brightens (5,301 pixels) and the UEF factory beside it doesn't
   (0).

Insect's and Metal's lookups read fixed game textures, which the test
can't replace (the game's archives are mounted first), so their formulas
are checked by review.
