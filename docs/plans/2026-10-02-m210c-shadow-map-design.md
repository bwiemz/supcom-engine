# M210c: Moho's shadow map

## Why

The engine's shadows are a conventional depth map: 4096², every caster's
depth, compared in hardware with 4×4 filtering, over a fixed square box
around the camera's focus. FA's are not, and they look different in ways a
player sees at once:

- **Hills never shadow the ground.** FA's terrain receives only the units'
  shadows; the engine's terrain shadows itself.
- **Unit shadows on the ground are soft blobs** of a blurred mask, not
  filtered depth comparisons.
- **Units' own shadowing is hard and coarse** (one point tap, a large bias)
  at Medium, and five depth taps at High.
- **Zoomed out past 250, there are no shadows at all.**

M211m gave the meshes FA's lanes and their shadow taps over the engine's
map. This replaces the map itself.

The rules come from faf-re's `Shadow` (`Init`, `PrepareLightCamera`,
`RenderShadowMap`, `RenderFrameShadows`), `MeshRenderer::ConfigureShader`
and `RenderDepth`, the terrain classes' `DrawTerrainDepth` and shadow
bindings, and FA's `terrain.fx` and `mesh.fx`. Retail's files are read for
the rules, never copied. The research notes, with every citation, are in
the session's `moho_shadow_research.md`; the rules below are their summary.

## The rules

### The map

- One colour target, N = `ren_ShadowSize` (1024), with a depth buffer.
  - **R** is the caster's light-space depth.
  - **G** is a mask: 0 where a mesh is the surface nearest the sun, 1
    elsewhere.
- Shadow fidelity 1 uses A8R8G8B8, 2 and 3 G16R16. At 1 nothing reads R
  (meshes receive only above 1), so one 16-bit two-channel format serves
  every level here.
- Every target is cleared to 1 in every channel, and the depth to 1, each
  frame. Every pass draws through a viewport inset by one pixel, so the
  border texels keep the clear value: a lookup off the volume reads "lit".
- Shadow fidelity 0 makes no map, and nothing is shadowed.

### The light camera

Each frame, from the main camera:

- **None** (and the map is cleared, so everything reads lit) when the
  camera's target zoom exceeds `ren_ShadowLOD` (250), or when no terrain is
  in view.
- **The light basis.** The light travels along L = −sun. The map's up axis
  is the camera's forward direction with its component along L removed, so
  the map turns with the camera's heading. When the two are nearly parallel
  (forward·L ≥ 0.99) it is (0, 0, −1).
- **The volume.** The main camera's frustum, its far plane moved to
  `ren_ShadowCoeff` × zoom (3 × zoom) past its near plane, intersected with
  the heightfield's min/max quadtree: the AABB of the terrain cells inside.
  Then its top is raised 8 units.
- **View and projection.** The eye sits 10000 units toward the sun from the
  volume's centre (right-handed look-at). The volume's eight corners in
  light space give an orthographic box fitted separately in x and y (not
  square, no texel snapping). Its near plane is the nearest corner's depth
  less 25 units; its far plane the farthest corner's. Depth is linear,
  0 to 1.

### The casters

Drawn depth-tested (less-equal, writing depth), with no rasterizer bias:

1. **The terrain** (`TTerrainDepth`), culling nothing. It writes
   G = 1 and R = z / 128: FA's vertex shader scales the whole homogeneous
   position, w included, by `HeightScale` (1/128), and the pixel shader
   writes the undivided z. So the terrain's R is about 0.
2. **The meshes** with a depth stage, culled as in the main pass (the
   light-facing surfaces are kept). Each writes R = its depth and G = 0.
   - `DepthClip` cuts by the albedo's alpha; `SeraphimBuildDepth` grows;
     `UndulatingDepthClip` sways: as the engine already does (M211f-j).

### The blur

When `ren_ShadowBlur` is on (the default), two passes, once:

- **Horizontal:** five point taps of G at −2…+2 texels, weights
  [1 4 6 4 1]/16, into target A.
- **Vertical:** four bilinear taps of A's G at ±0.5 and ±1.5 texels,
  weights {2, 6, 6, 2}/16, into target B.
- Each pixel samples at its texel's edge (D3D9's pixel centres): the net
  kernel is [1 5 10 10 5 1]/32 across by [1 3 3 1]/8 down, half a texel
  off. Both passes write their result to R and G.

With the blur off, the terrain reads the raw map's G instead.

### The receivers

- **The terrain** (the terrain shaders, the decals, the splats, the low
  fidelity lighting): `shadow` = the blurred map's G, sampled bilinear,
  white outside. No depth comparison, so the terrain never shadows itself
  and never takes the meshes' depth. Lit when there's no light camera.
- **The meshes**, above shadow fidelity 1 and off the Low lane (M211m), read
  the raw map's R at their light-space depth z:
  - one tap (`ComputeShadowStandard`): point-sampled, clamped; shadowed when
    z > R + 0.005;
  - five taps (`ComputeShadowPCF`, the High lane at fidelity 3 with the
    blur): bilinear R at (−½, 0), (0, −½), (−1, 0), (+1, 0) and (0, +1)
    texels, white outside; each lit when z < R + 0.006; their mean.
  - Since the terrain's R is about 0, a mesh pixel whose texel the terrain
    holds is shadowed: under a hill from the sun, and at a silhouette.
- No edge fades: off the volume reads lit.

The shadow scales only the sun's diffuse term, before the fill:
`light = multiplier × (sun × N·L × shadow + ambient) + fill × (1 − light)`,
as the engine already lights.

## The engine

- **`renderer/shadow_camera`:** the light camera as a pure function of the
  main view-projection, the camera's forward axis and zoom, the sun, and a
  `HeightBounds` min/max pyramid over the heightmap (built once per map).
  It returns no camera past 250 or with no terrain in view. Unit tests.
- **The map:** an R16G16 colour target plus a D32 depth buffer; blur
  targets A and B. The shadow render pass keeps its two dependencies on the
  shared map (write after the last frame's reads; reads after this
  frame's writes); the blur passes chain after it.
- **The casters' shaders** write (R, G). The terrain's pipeline culls
  nothing. The meshes' light projection flips y as the main camera's does,
  so the same cull keeps the same faces. The rasterizer depth bias goes.
- **The terrain mask** is always target B. With the blur on the two passes
  fill it; with it off, a copy of the raw G does. So the terrain's
  descriptor never changes.
- **The light UBO** gains the shadow's state: whether there is a light
  camera, the bias and the map's size.
- **The descriptors:** the raw map, point-sampled and clamped (binding 0);
  the raw map again, bilinear with a white border (binding 7, the PCF's);
  target B, bilinear with a white border (binding 8, the terrain's).
- **The placeholder cubes** (units without meshes, an engine-only fallback)
  cast with G = 0 and read like a Medium mesh.
- **Constants:** `ren_ShadowSize` 1024, `ren_ShadowLOD` 250,
  `ren_ShadowCoeff` 3 and the 0.005 bias are constants. `ren_ShadowBlur`
  is the existing convar.

## Left as it is, deliberately

- **Half-texel registration.** D3D9's pixel centres put FA's lookups up to
  1.5 texels off at the map's edges (0.5 at its centre). The blur keeps
  FA's half-texel shift; the map's rasterization and lookups use Vulkan's
  centres. The difference is about a texel.
- **Past `ren_ShadowLOD`,** faf-re's meshes keep sampling with the last
  light camera's matrix (or an identity before the first). Against the
  cleared map that would shadow meshes beyond the old volume's far plane,
  or all of them at a game's start. It is unverified in retail, so the
  meshes read lit here, as the terrain does.
- **The terrain drawn into the map** is the whole terrain, where FA draws
  the main camera's tessellation (off-screen terrain may be missing).
  Only the terrain's depth occlusion of meshes behind ridges depends on it.

## Tests

- **Unit (`test_shadow_camera`):**
  - no camera past zoom 250, or with no terrain in view;
  - the up axis follows the camera's heading;
  - the volume is the terrain in view, truncated at 3 × zoom, raised 8;
  - the near/far margins;
  - the HeightBounds pyramid's AABB query agrees with a brute-force scan.
- **Integration (`--shadow-map-test`):**
  - a hill never shadows the ground;
  - a unit's shadow on the ground is soft and fades at its edge (the blur),
    hard with `ren_ShadowBlur` off;
  - past zoom 250 no shadow;
  - the terrain shadows a unit behind a ridge;
  - shadow fidelity 0: nothing.
- The existing shadow tests move to the new rules where they measured the
  old map's look.
- The goldens change (no terrain self-shadowing); re-recorded.
