# Cartographic view (the minimap's map) — design

Status: proposal, 2026-10-13. Milestone number provisional (next free after M228).
Sources: faf-re `render/Cartographic.{h,cpp}` (read in full), retail `cartographic.fx` (read in full), retail `minimap.lua`, `multifunction.lua`, `controls/worldview.lua`, `game/worldview.lua`, `gamemain.lua`. Secondary, fetched from Draiget/faf-re `master` through a summarizer (so lower confidence): `CWldMap.cpp` (`CWldTerrainRes::Load`), `Mesh.cpp` (`MeshRenderer::RenderCartographic`), `CameraImpl.cpp`, `CUIWorldView.cpp`, `WRenViewport.h`. Retail's files are read for the rules, never copied; `cartographic.fx` is not shipped, the shaders below are hand-ports.

Confidence tags: **[R]** read directly in the local decompile; **[S]** from a fetched faf-re file; **[I]** inferred; **[?]** doubtful or unverified.

## Why

FA draws its minimap, and (optionally) its main view, as a "cartographic" map: a flat, hypsometric-tinted terrain (water in three blues, land blending green to white) with topographic contour lines, instead of the lit 3D world. The engine's minimap is a CPU-painted 256² gradient that has no source in Moho. The `SetCartographic` flag is accepted and ignored. Retail's own minimap window calls `SetCartographic(true)`, so every retail game currently shows the wrong picture.

## 1. Moho, stage by stage

### 1.0 What "cartographic" is, and who draws it

- Lua `CUIWorldView:SetCartographic(bool)` (`cfunc_CUIWorldViewSetCartographic` 0x00873170 / `...L` 0x008731F0) "updates one world-view orthographic/cartographic render mode flag"; `IsCartographic` (0x008732C0 / 0x00873340) "returns whether one world-view currently renders in orthographic mode" [R: faf-re `ui/UiRuntimeTypes.h`]. The flag is `CUIWorldView::SetOrthographic` (0x0086DC00), which stores it, calls `CameraImpl::CameraSetOrtho` (0x007A6A10, stores `mIsOrtho`), and `CanShake(!ortho)` [S].
- `CameraImpl::UpdateCoords` (0x007AA330) [S]: when ortho, the view is a fixed top-down look (`COORDS_Orient(pi, pi/2)`), the eye is `mOffset + forward * eyeDistance`, no shake. The projection is orthographic: `r[0].x = 2/targetZoom`, `r[1].y = 2/verticalExtent` (`verticalExtent = targetZoom / aspect`), `r[2].z = 1/(far-near)`, `r[3].z = near/(near-far)`. Shared with perspective: `eyeDistance = targetZoom / tan(mFarFov/2) / 2`, `near = max(0.01*d, 0.01)`, `far = d + 17000`. The decompile shows `r[3].x = -1/(int)zoom` and `r[3].y = +1/(int)extent`; that reads as a recovery slip for the symmetric-frustum offset 0 [?].
- `WRenViewport::RenderCartographic` (0x007F8BA0) "renders the cartographic (map-mode) views of a head; returns how many it drew" [S: WRenViewport.h]. It calls `Cartographic::Initialize` lazily (call site 0x007F8C45) and `Cartographic::Render` (call site 0x007F8DCD) [R: Cartographic.cpp comments]. The other views go through the normal `WRenViewport::Render` (0x007F90D0).
- Retail uses it three ways:
  - The minimap window always: `minimap.lua:115-128` creates `WorldView(..., 'MiniMap', 2, true, 'WorldCamera')`, then `SetCartographic(true)`, and after a frame `CameraReset()` and `GetCamera('MiniMap'):SetMaxZoomMult(1.0)` (so the map fills the window with no margin).
  - The main view: the `Cartographic` toggle in the map-options menu for the main world cameras (`multifunction.lua:445-450`, saved as `<camera>_cartographic_mode`, restored in `controls/worldview.lua:587`), and the `ToggleMainCartographicView` function (`game/worldview.lua:191`).
  - Cinematics turn it off and restore it (`gamemain.lua:546-571`).

### 1.1 The inputs

- **Per-map, in the `.scmap`** [S: `CWldTerrainRes::Load` 0x008A1700]: for version >= 0x38 (56) it reads `mTopographicSamples` (int) then `mHypsometricColor[0..4]` (ARGB uint32), after the water properties and wave generators and before the texturing block; for version >= 0x39 (57) it reads `mImagerElevationOffset` (a float; used by `MapImager`, not by `Cartographic`). For version < 0x38 it sets samples = 20 and colours `0xFF0E3EFF, 0xFF215CFF, 0xFF4785FF, 0xFF4C9D32, 0xFFFFFFFF`. Version >= 0x3B (59): `mCartographic.ReadDecals(version, reader)` after the skydome section.
  - The colours are: [0] abyss blue (14,62,255), [1] deep blue (33,92,255), [2] shallow blue (71,133,255), [3] land low green (76,157,50), [4] land high white. P1's survey: 53 of retail's 60 maps store [1] as grey `0xFF707070` (the FAF map editor calls it the contour colour) and the other 7 Moho's default; SCMP_009's are `FF478CB5 FF707070 FF8CC9E0 FF75636B FFCECEB0`, 24 bands.
  - The engine's parser skips this block as 28 bytes from version 59 and 24 for 54-58 (`scmap_parser.cpp:254-263`). faf-re says 24 bytes from 56 and 28 from 57, and the FAF map editor (ozonexo3/FAForeverMapEditor `Map.cs`) reads the same, so phase 1 follows them: a v54/55 map has no block, a v57/58 map has the offset. All 60 retail maps are version 60 (one more is 51), so only user maps change.
- **From the heightfield** (`Cartographic::InitializeTerrain` 0x007D20F0 [R]): `gridW = (W-1) >> 1`, `gridH = (H-1) >> 1`, where W,H are the heightfield's sample counts (map size + 1). `tierBox = heightField.GetBounds3D()` is the top tier's box: `elevMin = Min.y`, `elevMax = Max.y`, in world units (raw word / 128). Whether the box is exactly the heights' min/max is not shown [?].
- **From the map's water** (`STIMap`: `mWaterEnabled`, `mWaterElevation`, `mWaterElevationDeep`): `hasBounds = waterEnabled`.
  - With water: `surface = waterElevation`; `water = waterElevationDeep`; `default = waterElevationDeep`.
  - Without water: all three = `elevMin`.
  - The decompile reads the *deep* value for both the `water` and `default` lanes (comment at lines 1735-1748). The abyss value is the natural third lane; the decompile may have mis-resolved an inlined accessor [?]. This decides whether palette [1] ever shows (see 1.3).
- **Coefficients**:
  - `gridSizeCoeff = {1/gridW, 1/gridH, 0, 1}`
  - `terrainSizeCoeff = {gridW/(W-1), gridH/(H-1), 0, 1}`
  - `terrainHeightScale = 1/128` (0x3C000000).

### 1.2 Resources built at Initialize

`Initialize` 0x007D13E0 = `InitializeFrame` (0x007D1E90: a 4-vertex dynamic frame quad buffer and a 6-index quad index buffer) then `InitializeTerrain`, then `mInitialized = true`; any exception calls `Shutdown` (0x007D14B0). It is built once, from the heightfield as it is then. A later flatten does not rebuild it (no refresh call exists) [I].

`InitializeTerrain` also uploads the ground quad: 4 vertices of 4 x int16 `(x, elevWord, z, 1)`, with x,z = 0 or `W-1`/`H-1`, `elevWord = int(surface * 128)`; so the quad lies flat at the water surface (or the lowest point) and covers the whole map.

### 1.3 The three lookup textures (`InitializeTerrainTextures` 0x007D24F0)

Let `span = elevMax - elevMin`, `inv = span > 0 ? 1/span : 1` (the guard may be the recoverer's [?]), and `nS = (surface-elevMin)*inv`, `nW = (water-elevMin)*inv`, `nD = (default-elevMin)*inv`.

1. **Elevation** (`mElevTexture`, GAL format token 14, `gridW x gridH`, one mip, static). Texel `(x,z)` holds half-float `E(sampleX-1, sampleZ-1)`, where `sampleX = int(x * (W-1)/(gridW-1))`, `sampleZ` likewise, and `E = (word/128 - elevMin) * inv` (`SampleCartographicHeightKernel` 0x007D0F70). The sample index is clamped to the field after the `-1` shift. The weights `{0,1,0, 1,3,1, 0,1,0}/7` are applied to the *same* texel nine times in the decompile, so the kernel collapses to a single sample [R, flagged in the source; possibly a decompile artifact [?]]. Only the first 16-bit word of each 2-word texel is written (the shader reads `.r` of `.rg`); the second is never written. Conversion uses `D3DXFloat32To16Array` (call at 0x007D2759).
2. **Hypsometric ramp** (`mHypsometricTexture`, token 2, 256x1; the shader reads `.rgb`, so ARGB8). For texel `i`, `n = i/255`:
   - without `hasBounds`: `blend(c3, c4, (n-nS)/(1-nS))` (here `nS = 0`, so `t = n`);
   - with it: `n <= nD` -> c0; else `n <= nW` -> c1; else `n <= nS` -> c2; else `blend(c3, c4, (n-nS)/(1-nS))`.
   - `blend` (`BlendPackedColor` 0x007D0E60) works per byte, all four: `int((to-from)*t + from) & 0xFF`, truncating toward zero.
   - With default == water (the decompile), c1 never appears: the water is two-tone (c0 at or below deep, c2 from deep to surface).
3. **Topographic ramp** (`mTopographicTexture`, token 5, 256x1; the shader reads `.a`, so an 8-bit alpha format). `bands = clamp(samples, 1, 100)`; `landSpan = 1 - nW`; `step = landSpan/bands`; `ceil[0] = step + nW`, `ceil[k] = ceil[k-1] + step`. For texel `i`:
   - with `hasBounds` and `nW >= n`: 0;
   - else the first band `b` with `ceil[b] >= n` (the decompile's loop runs to `bands`, where `ceil[bands]` is zero or stale; the highest texel can land there in float [?]); value `clamp(int(((ceil[b]-nW)/landSpan)*239 + 16), 16, 255)`.
   - So water is 0 and each band is a flat shade between 16 and 255.

Sampling (cartographic.fx) is POINT everywhere with CLAMP. A `sampler1D` lookup at `h` picks texel `floor(h*256)` (clamped to 255) while the ramps are built at `i/255` (a half-texel-class misalignment; faithful quirk).

### 1.4 The frame (`Cartographic::Render` 0x007D17C0) — the order [R]

1. Force `ui_AlwaysRenderStrategicIcons = true` (restored at the end), `ClearTextures`.
2. Compute the world view's viewport from row 3 of the camera's viewport matrix (truncated to integers), and the head's full viewport.
3. Bind the caller's offscreen colour + depth/stencil pair (the decompile calls this `ClearTarget`; the same call rebinds the head later, so it is a render-target set), set the view viewport.
4. **Stage0** (`RenderTerrainStage0` 0x007D2EA0): clear the offscreen frame to opaque black, depth 1, stencil 0; draw the ground quad with technique `Terrain_Stage0` (one pass).
5. If a world session exists: `RangeRenderer::Render(session, camera, head, deltaFrame)` (if a range renderer and camera exist), `RenderFogOfWar` (the caller passes the vision renderer only when `ren_FogOfWar` is on), `RenderPlayableBoundary` (only when `ren_PlayableBoundary` is on). They draw into the offscreen frame, over the terrain, so their output goes through the edge detect.
6. Rebind the head's output context, same viewport.
7. **Stage1** (`RenderTerrainStage1` 0x007D3580): clear the head (colour black, depth 1, stencil 0); draw a pre-transformed quad covering the head's whole render target, half-texel corrected, `z = rhw = 1`, with technique `Terrain_Stage1`; `frameWidth/Height` = the head target's size, `frameTexture` = the offscreen colour target.
8. **Meshes** (`RenderMeshes` 0x007D2CB0), then **particles** (`RenderParticles` 0x007D2E40: `sWorldParticles.RenderEffects(camera, renderWaterSurface=0, suppressTLight=1, tick, alpha)`), then every **decal batch** (`CartographicDecalBatch::Render` 0x007D50D0), then, with a session, `RenUI(session, camera, primBatcher, deltaFrame, 0.0f)`.
9. `Clear(depth+stencil only)` then `worldView->Render(primBatcher, head, deltaFrame, 0.0f)` (slot 0: strategic icons, resource splats, projectile icons, command graph, economy readout), then `REN_DebugStuff`, then restore the full head viewport.

No sky dome, water, shadow pass, terrain textures or lighting is drawn. Stage1 writes alpha 0, so the cartographic terrain adds no glow to the bloom; the decal glow pass and particles can. The head's depth is cleared by Stage1, so meshes never see the terrain's depth.

### 1.5 Stage0: the terrain (`Terrain_Stage0`) [R: cartographic.fx]

- State: `AlphaBlend_Disable_Write_RGBA`, `ZEnable = true`, `ZWriteEnable = true`, `ZFunc = always`. Culling is unspecified (D3D9 default CCW); the quad is always seen from above in ortho. Vertex shader `vs_1_1`, pixel shader `ps_2_0`.
- The ZWrite is on, so the range overlay's z-fail volumes depth-test against the flat quad at the surface elevation, not the real relief.
- `TerrainVS`: `p = float4(short4)`; `p.y *= terrainHeightScale`; `world = p.xyz`; `position = p * view * proj`.
- `TerrainPS0`:
  - `v0 = terrainSizeCoeff.xy * world.xz`; `t = frac(v0)`;
  - four point taps of the elevation texture at `gridSizeCoeff.xy * (v0 + {0,1}x{0,1})` (texel `floor(v0)` plus 0/1, clamped);
  - `h = clamp(bilerp(h0..h3, t) - 0.0001, 0, 1)` (using `.r`);
  - output `float4(hypsometric[h].rgb, topographic[h].a)`.
  - So the colour is the ramp colour at the bilinear elevation, and alpha carries the quantized contour band.
- The cartographic frame is therefore 8-bit-class RGBA in D3D (the formats are not visible in the decompile) [?]. The uniforms `elevMaximum/elevMinimum` are bound but unused by `TerrainPS0`.

### 1.6 Stage1: contours (`Terrain_Stage1`) [R]

- State: `AlphaBlend_Disable_Write_RGBA`, `Depth_Disable`; fixed-function vertex stage with the pre-transformed quad.
- `TerrainPS1`: `dx = 1/frameWidth`, `dy = 1/frameHeight`; the centre `color = tex(uv)`, `c = color.a`; the eight neighbours' alpha at `+-dx, +-dy` (all point, clamp); `topo = saturate(16 * (c - 0.125*sum8))`; output `float4(color.rgb - topo, 0)`.
- A band boundary darkens only the pixels on the higher side. A straight edge with a band step of s gives `16*0.375*s`; the shore (land at the first band's shade 28/255 against water 0) gives about 0.66, a strong dark line. The result is clamped by the 8-bit target.
- The output alpha is 0.

### 1.7 Meshes [S: `MeshRenderer::RenderCartographic` 0x007DFF30, forwarder 0x007E0380; `Mesh.cpp`]

- `RenderMeshes` binds this view's hypsometric ramp to the `hypsometricTexture` variable of the shared `"mesh"` effect, makes that effect current, rebuilds the global batch map (`MeshRenderer::Batch(gameTick, deltaFrame, camera, camera.viewport.r[1])`: the normal spatial-DB gather with frustum cull, LOD, dissolve and the skinned/static gates), then calls `RenderCartographic(surfaceElevation, elevMinimum, elevMaximum, camera, buckets)`.
- It does not select an effect (it reads `device->GetCurEffect()`), and sets no blend or depth state itself; the technique's own pass states apply.
- Per frame it binds `time` (frame counter + deltaFrame, mod 36000), `lodBasis` (`camera.viewport.r[1]`), `minimumElevation`, `maximumElevation`, `surfaceElevation`, `viewMatrix`, `projMatrix` (no mirroring). It binds no shadow, lighting or terrain uniforms.
- Per material: the technique named by the shader annotation **`cartographicTechnique`** (cached in `MeshMaterial::mCartographicTechnique`, default empty). A material with an empty name is not drawn. Samplers bound: albedo, specular, normals, in that order.
- Buckets are walked in the batch map's key order (`isStaticPose`, LOD, `Mesh::GetSortOrder`); each draws via the LOD's skinned (static-pose) or static batch with `Render(bucket, false)`.
- **Not known**: which mesh.fx techniques carry the annotation (units only? props?), and what the pixel shaders do with the ramp and the elevation range (probably tint by height). That needs `/effects/mesh.fx` from retail. The engine has no cartographic mesh shader and has not ported such techniques (`grep -i cartographic src/renderer` finds nothing).

### 1.8 Cartographic decal batches [R]

- File format (`CartographicDecalBatch::Read` 0x007D5400, after the sky block, count then batches): technique string (version >= 60; else `"Decal"`), texture path string, int count, then per decal 9 floats = 36 bytes: `{x, z, rotation, sizeX, sizeZ, u, v, w, h}` (`DecalVS`: `position.xy` is world x,z, `position.z` the angle, `size`, `texcoord` the atlas rectangle).
- Drawing (0x007D50D0): lazily create the resources (0x007D56C0: a 4-corner quad stream, a 1024-instance dynamic buffer of 9 floats, a 6-index buffer {0,1,2,3,2,1}), lazily resolve the texture (0x007D59C0), upload dirty instances (0x007D5AD0), bind `viewMatrix`, `projMatrix`, `decalTexture`, draw one instanced quad per decal. The technique is the batch's own name: `Decal` or `DecalGlow`.
- `DecalVS`: `uv = tex.xy + 0.5*tex.zw*(corner+1)`, `uv.y = 1-uv.y`; `r = rotate(size*corner, angle)`; `p = pos.xy + (r.x, 0) + (0,-r.y)`; world `(p.x, 0, p.y)`.
- States: `Decal`: `CullMode = None`, `AlphaBlend_SrcAlpha_InvSrcAlpha_Write_RGB`, `Depth_Disable`. `DecalGlow`: same, `..._Write_A` (alpha only, which is the glow channel). The sampler is linear with mips, clamped.
- The batches draw after the edge detect, so contours never touch them. A batch with more than 1024 decals overruns Moho's instance buffer [?].

### 1.9 Doubts in the decompile (all [?])

| # | Doubt | Effect |
|---|---|---|
| 1 | `mDefaultElevation` and `mWaterElevation` both read from the deep elevation | whether the third water blue appears. P1: most retail maps store grey in [1], so [1] showing as a water band is unlikely: the decompile's reading is kept |
| 2 | The 3x3 kernel collapses to one sample (weights applied to one texel) | smoothing of the elevation texture |
| 3 | Tier box = exact min/max of the heights? | all ramp positions |
| 4 | Ortho `r[3].x`, `r[3].y` read as `+-1/(int)zoom` | main-view framing only |
| 5 | Topographic loop can index `ceil[bands]`, which faf-re's port zero-fills (the binary's stack slot may be stale) | only the highest texels; the port reads zero, as faf-re's does |
| 6 | Version gates for the tileset block: settled as 56/57 (faf-re and the FAF map editor agree) | user maps only |
| 7 | GAL format tokens 2, 5, 14 are inferred from how the shader uses them | storage only |
| 8 | `degenerate span` guard (`span > 0`) | flat maps only |
| 9 | Range/fog blend states and whether they write the frame's alpha | contour lines under overlays |

## 2. The engine today

- **The flag**: `WorldView::cartographic_` (`src/ui/world_view.hpp:55-56,80`), default false. It is set by Lua `SetCartographic` (`src/lua/bindings/ui/world.cpp:407-412`) and read by `IsCartographic` (`world.cpp:382-386`). Nothing in `src/renderer` reads it. A test only checks that the stubs don't crash (`integration_tests.cpp:20280-20285`). Retail's minimap sets it true, so a retail game reports `IsCartographic() == true` on the minimap and renders nothing differently.
- **The minimap view**: `worldview_init` (`src/lua/user_bindings.cpp:211-261`) makes a `ui::WorldView`; `is_minimap` comes from the 5th argument (line 243). `bind_world_view` (`193-209`) binds every WorldView, minimap included, to the renderer's one camera (named "WorldCamera"); only a non-minimap view becomes `__osc_world_view`. There is no minimap camera.
- **How it is drawn**: not a world view rendered small. `UIRenderer::collect_control` (`ui_renderer.cpp:658-674`) calls the minimap painter for an on-screen minimap view; `Renderer::update_frame_scene` (`renderer.cpp:2690-2709`) wires it to `MinimapRenderer::paint` (`minimap_renderer.cpp:289-302`), which emits UI quads in the `WorldContent` band: the terrain texture, resource icons, the strategic icons (`StrategicIconRenderer::paint_map`), and the camera outline.
  - The background is one 256² RGBA texture `__osc_minimap_terrain`, built on the CPU from the heightmap (`build_terrain_texture`, `minimap_renderer.cpp:44-116`; called from `renderer.cpp:1577-1578`, evicted at `renderer.cpp:1408-1411`): blue shades below the water, a green-brown ramp by height.
  - The map is fitted into the view by `fit_map_area` (`minimap_renderer.cpp:118-124`; the letterbox margin is left undrawn). Clicks map back through `minimap_to_world` (`126-134`) and `hit_test` (`411-415`).
  - The C++ HUD has a second path (`update`, `renderer.cpp:2803-2807`; `render`, `renderer.cpp:3125-3130`) using the same quads.
- **Frame structure** (`Renderer::render`, `renderer.cpp:1898-2046`): shadow pass, normal pass, reflection pass, `record_main_pass` into the HDR scene target (`frame_targets.cpp`, `R16G16B16A16_SFLOAT` + the shared depth/stencil), bloom, then a swapchain pass (`B8G8R8A8_UNORM`, `renderer.cpp:652`) where `record_screen_layers` draws the icons and UI quads. The main world view "fills the scene underneath all UI" (`ui_renderer.cpp:739-744`). Only one world is ever rendered.
- **Reusable pieces**: `SkyRenderer` (`sky_renderer.{hpp,cpp}`) is the shape for a module with its own pipelines, samplers and per-frame uniforms; `ShadowMap`/`FrameTargets` show self-contained offscreen targets and render passes whose dependencies match the scene's so existing pipelines draw in them (`frame_targets.cpp:101-165`); `TextureCache::make_descriptor(VkImageView)` (`texture_cache.hpp:74`) makes a UI-samplable descriptor for an externally made image; shaders are embedded GLSL strings compiled at start (`shader_utils.cpp`, e.g. the sky's at 1255-1291; `compile_glsl` targets Vulkan 1.0).
- **The map data**: `skip_cartographic_decals` (`scmap_parser.cpp:162-179`) parses and discards the batches; the tileset block (samples, 5 colours, imager offset) is skipped by size (`scmap_parser.cpp:254-263`). `ScmapData` (`scmap_parser.hpp:173-208`) holds neither. `Terrain` (`terrain.hpp`) has `water_elevation()`, `has_water()`, `water().`, `water_abyss_elevation()`, `heightmap()`; `Heightmap` knows only its max (`heightmap.hpp:34`) and exposes `get_height_at_grid` (raw word * scale). No min.
- **Tests that touch it**: `data.golden.scmp009_game_ui_minimap` (`tests/integration/CMakeLists.txt:497-507`, with `fixtures/minimap.prefs`) will change; `test_minimap_area.cpp` and `strategic_icon_test.cpp` use `fit_map_area` & co.

## 3. The port

### 3.1 Decision

Add a self-contained `CartographicRenderer` that renders the cartographic frame to its own offscreen image; the minimap samples that image where it now samples the CPU texture. Keep the existing geometry (`fit_map_area` -> view rect), icons and hit-testing. Do not try to make the engine render "a second world view": there is one camera and one scene pass, and the cartographic frame needs neither. Ortho for the minimap is built from the map rectangle, not from a `Camera`. A real ortho `Camera` path is needed only for the main view (phase 7).

Rejected: reuse the scene pass with a mode switch (the minimap needs a second image at another size in the same frame); render icons into the frame (they stay UI quads, which is what the engine already does and what hit-testing relies on).

### 3.2 Data (phase 1)

- `map::ScmapCartographic { i32 topographic_samples = 20; u32 hypsometric[5] = {0xFF0E3EFF, 0xFF215CFF, 0xFF4785FF, 0xFF4C9D32, 0xFFFFFFFF}; f32 imager_elevation_offset = 0; }` and `map::ScmapCartographicBatch { std::string technique = "Decal"; std::string texture; std::vector<std::array<f32,9>> decals; }`. `ScmapData` gains `cartographic` and `cartographic_batches`. `scmap_parser.cpp`: parse the block in place of the skip at 254-263, and `skip_cartographic_decals` becomes `read_cartographic_decals`. The older-version defaults are Moho's. Reconcile the 54-59 thresholds against faf-re's (1.1).
- `Terrain` gets `set_cartographic(...)`/`cartographic()`/`cartographic_batches()` (as `set_sky`, `terrain.hpp:136-138`), and `renderer.cpp`'s `build_scene` hands them on as it does the sky.
- No `Heightmap::min_height()`: `cartographic_params` takes the floor and ceiling from the current grid itself (`Heightmap::max_height()` only ever grows, so a flatten that lowered the top would leave it stale). `Terrain` keeps the deep water's start (`set_water_deep_elevation`), which it lacked.

### 3.3 Pure math: `renderer/cartographic_math.{hpp,cpp}` (phase 1)

No Vulkan. Functions:
- `CartographicParams make_cartographic_params(const Terrain&)`: gridW/H, the two coefficient vectors, `elev_min/max`, `has_bounds`, `surface/water/default` (the deep/deep rule in a single function with a named constant for the abyss alternative), the palette and the band count.
- `std::vector<u16> build_elevation_texels(const Terrain&, const CartographicParams&)` (half-float words via a local `float_to_half`, round-to-nearest as the D3DX fallback in the decompile; single-sample kernel; the `-1` shift and clamp kept).
- `std::array<u32,256> build_hypsometric_ramp(...)` (including `blend_packed_color`), `std::array<u8,256> build_topographic_ramp(...)` (reading the zero `ceil[bands]` past the last band, as faf-re's port does).
- (Moved to P2, where it is first used) `cartographic_ortho(...)`: the matrix for a map rect and eye height, with Moho's near/far (`near = max(0.01*d, 0.01)`, `far = d + 17000`, `d = zoom / tan(fov/2) / 2`, fov 60 degrees) so range-volume depths behave.
- `struct CartographicDecalInstance` is the 9-float record.

### 3.4 `CartographicRenderer` (phase 2)

File `src/renderer/cartographic_renderer.{hpp,cpp}`, shaders in `cartographic_shaders.{hpp,cpp}` (the engine has `shield_shaders.cpp` as precedent; the sky's live in `shader_utils.cpp`). Added to `src/renderer/CMakeLists.txt`.

- `init(device, allocator, scene_pass_formats, depth_format)`: samplers (nearest-clamp for the frame; linear-clamp for decals), descriptor layout, pipelines, per-frame uniform buffers.
- `build(const Terrain&, TextureCache&)`: params, the three textures, decals. `clear()` on map change (the renderer's `clear_scene`).
- `ensure_target(fi, w, h)`: two images per frame in flight: **frame** (stage-0 output) and **output** (stage-1 output; later the target for meshes/decals/overlays), both `R16G16B16A16_SFLOAT` plus their own depth/stencil of the frame depth format. Recreated, with deferred destruction, when the size changes.
- `record(cmd, fi, view)`: outside any render pass, in the frame's command buffer before the shadow pass: pass A (frame) then pass B (output).
- Two render passes, each with the scene pass's attachment formats and dependencies, so the scene's existing pipelines (range overlay, particles, mesh) are render-pass compatible [I; to be verified in phase 5]. Output ends in `SHADER_READ_ONLY` for the UI.
- Pipelines:
  - **terrain** (`Terrain_Stage0`): no vertex buffer, 4 corners from `gl_VertexIndex` and the uniform; depth test ALWAYS + write; no blend; write RGBA; cull none.
  - **edge** (`Terrain_Stage1`): fullscreen triangle; no depth; no blend; write RGBA.
  - **decal** and **decal glow** (`Decal`, `DecalGlow`): the instanced quad; cull none; no depth; blend SrcAlpha/InvSrcAlpha on colour *and* alpha as D3D9 does without a separate alpha blend (as the sky glow pass already does in `sky_renderer.cpp:237-246`); mask RGB or A.
- Descriptors: elevation (R16_SFLOAT, nearest, clamp, `texelFetch`), hypsometric (`B8G8R8A8_UNORM` taking the ARGB ints verbatim), topographic (`R8_UNORM`, read `.r`), frame (the stage-1 input). Needs a format-parameterised upload next to `TextureCache::upload_rgba` (an `upload_pixels(key, VkFormat, bytes, w, h)`); the half-float G channel Moho never writes is not stored.
- Hazards: per-frame-in-flight images, so frame N+1's writes cannot overlap frame N's UI read (the shadow map handles the same with dependencies; here duplication is cheaper than reasoning).
- Cost: one quad and one fullscreen pass at the minimap's size; negligible.

### 3.5 GLSL ports (specifications; not final code)

All four techniques, hand-ported from `cartographic.fx`; Vulkan 1.0 GLSL 450.

- **Terrain_Stage0** — vertex: corners `(0|W, surface, 0|H)` from the index; `gl_Position = viewProj * vec4(x, surface, z, 1)`; pass world xz. Fragment:
  - `v0 = terrainSizeCoeff.xy * world.xz; base = ivec2(floor(v0)); t = fract(v0);`
  - four `texelFetch(elev, clamp(base + off, 0, size-1), 0).r`; `h = clamp(mix(mix(h0,h1,t.x), mix(h2,h3,t.x), t.y) - 0.0001, 0, 1);`
  - `i = min(int(floor(h*256.0)), 255);` `out = vec4(texelFetch(hypso, ivec2(i,0), 0).rgb, texelFetch(topo, ivec2(i,0), 0).r);`
  - `texelFetch` replaces the point sampler (the same texel, without the rounding risk of `gridSizeCoeff * v`).
- **Terrain_Stage1** — fullscreen triangle, `uv = gl_FragCoord.xy / size` (pixel centres: the half-texel correction of D3D9 becomes unnecessary). `d = 1/size`; nearest-clamp sampler. `topo = clamp(16.0 * (c - 0.125 * sum8), 0, 1); out = vec4(clamp(color.rgb - topo, 0, 1), 0)`. The output is saturated explicitly: Moho's target is 8-bit, the HDR target would keep negatives (the sky doc makes the same point).
- **Decal / DecalGlow** — `DecalVS` as 1.8 with `viewProj`; the fragment is `texture(decalTex, uv)`. DecalGlow writes only A (the pipeline's mask).
- The Vulkan clip space needs no flip for the minimap: with NDC y down, `ndc = (2x/W-1, 2z/H-1)` puts z = 0 at the top as the engine's minimap does already.

### 3.6 How the minimap switches

- `MinimapRenderer::paint`/`update` (`minimap_renderer.cpp:289-302,148-178`) take a `std::optional<CartographicTarget>` (the descriptor and the size). `build()` (`304-371`) emits the cartographic quad instead of `terrain_ds_` when it is given one; otherwise the CPU texture, so a headless run, `SetCartographic(false)` and a map with no data keep working. The resource icons, strategic icons and camera outline are unchanged.
- The painter lambda in `renderer.cpp:2691-2709` knows the view (`wv.is_cartographic()`) and its rect `r`. It passes the cartographic target only when the flag is set. It also records `{w, h}` = the rect snapped to whole pixels in the renderer for the command recording that follows, because the render happens later in the same frame (`update_frame_scene` precedes `begin_commands`). *As built (P3):* the snap truncates Left, Top, Width and Height each, minimum 1, because those are the pixels the UI renderer clips the view to (`collect_control`'s `self_clip`); rounding left the frame's last column and row clipped away. The target is made (`CartographicRenderer::prepare`) in the painter, not at record time, so its descriptor is valid when the UI quads are built.
- The frame covers the whole view rect, not only the fitted map area: the part outside the map stays opaque black as Moho clears it (the engine now leaves the margin undrawn). The view rect is drawn 1:1 with a point sampler (a new `make_descriptor(view, sampler)` overload) so 1-pixel contour lines are not blurred by resampling. *As built (P3):* the renderer owns one UI descriptor set per frame in flight instead (no set left in the texture cache's pool per resize), sampling a second view of the output whose alpha swizzles to 1: Stage 1 writes alpha 0, which Moho draws straight into the view, but the UI blends and discards alpha under 0.01.
- `Renderer::record` calls `cartographic_.record(cmd, fi, view)` before the shadow pass when a cartographic minimap exists this frame; the C++ HUD minimap (`legacy_hud_active_`) may use the same target, with the same flag test on its own placeholder (set true).
- `dump_frame` (`renderer.cpp:2172-2220`) gets a `cartographic` section (size, flag, descriptor present).
- The framing: area = the largest map-aspect rect, which equals Moho's `SetMaxZoomMult(1.0)` zoom for the minimap (`zoom = max(mapW, mapH*aspect)`), so ortho extents are `mapW x mapH` per axis within the fitted area; the black margin handles the rest.

### 3.7 Meshes (phase 6, gated)

Not implementable from what is on hand. First extract `/effects/mesh.fx` from retail (as `cartographic.fx` was), list the techniques with a `cartographicTechnique` annotation and their pixel shaders. Then:
- Add a mesh pipeline variant `MeshTechnique::Cartographic` in `renderer.cpp`'s mesh pipelines (`create_mesh_pipelines`, `renderer.cpp:1040`) that draws the same batched instances (the engine's `draw_meshes`, `renderer.cpp:2257`) with the cartographic vertex/fragment, push constants `surfaceElevation`, `elevMin`, `elevMax`, `lodBasis`, `time`, and the ramp bound as a descriptor; the albedo/specular/normal order is Moho's.
- Draw them after pass B's edge detect, in its depth-tested pass, with the engine's existing Batch cull.
- Only materials with a non-empty `cartographicTechnique` draw.
- Meshes get no terrain depth, no shadows, no lighting from the scene.
- Minimap meshes are a few pixels each and the icons cover them; the visible effect is mostly the main view, so this phase belongs with phase 7.

**As built (P6, 2026-10-15), the main view's meshes.** From retail's `mesh.fx`, read from `effects.scd`:
- **The technique.** It is the `cartographicTechnique` annotation of the lane the fidelity resolves to (High uses Medium, Medium and Low use each other). `cartographic_technique(shader, fidelity)` holds the table, and each GPU mesh caches it for the three lanes from its resolved shader name. The engine's `MeshTechnique` folds several shaders into Unit, so it can't be used.
  - Units: Unit, Metal, Insect, Aeon, AeonCZAR, Seraphim and SeraphimPersonalShield.
  - Features: props, trees, wrecks and NormalMappedTerrain.
  - Feedback: order marks and rally points.
  - Place: build and formation ghosts, at every fidelity, since only a Low lane exists.
  - Build: the four build techniques.
  - Shield: the faction shields and PhaseShield. So a UEF, Cybran or Aeon commander wearing its phase-shield mesh (its warp-in) draws as a highlight.
  - At Low, NormalMappedAlphaNoShadow and BloatingNormalMappedAlpha are Unit.
  - Everything else is not drawn.
- **The shaders.** `CartographicVS`: the world matrix alone, no growth, crumple, sway or feedback scaling. Then the six pixel shaders, and five pass states (colour, glow, opaque, opaque with alpha, shield), culled as the meshes are.
- **The draw.** After Stage 1, in the scene pass, over the frame. The meshes are the main view's own batch (frustum, LOD, intel), not drawn at the strategic zoom, as `draw_meshes`.
- **Not yet:** the minimap's meshes. They need a gather of their own over the whole map, and Moho's LOD cutoffs leave few at that zoom.

**As built (2026-10-17), the minimap's meshes.** From faf-re `Cartographic::RenderMeshes`, `GeomCamera3::Init`, `Mesh::ComputeLOD` and `RMeshBlueprintLOD`, and FAF's `MinimapMesh.cpp` patch:
- **The gather.** `RenderMeshes` batches every mesh through the minimap's own camera, so the engine gathers apart from the world view: a second `UnitRenderer`, with its own instances and bones, culled by the minimap frame's volume.
- **The LOD.** The minimap camera is orthographic, so its LOD metric is the same everywhere: the view's width times its LOD scale. A minimap view's camera takes `cam_DefaultMiniLOD` (1.8) when the view is made, and keeps it if the convar changes later.
  - A mesh shows while some LOD takes the metric. A LOD with a cutoff of 0 or less takes any metric; any other LOD takes metrics up to its `LODCutoff`, which defaults to 1000 where the blueprint gives none.
  - The reach is read from the mesh blueprint's LODs, because the engine's loaded LOD set can be a single-LOD fallback.
  - Unit counts, from retail's blueprints: 89 of 563 units on 5 km maps, 15 on 10 km, none on 20 km. But some meshes reach any metric: a ping's (99999), and those with `LODCutoff = -1`, such as the cruise missile, the nuke's stem, explosions, build effects and DevTest props. Every map's minimap therefore batches its entities each frame, as Moho's does. FAF's patch comment calls this "thousands of useless LOD calculations".
  - Which LOD a shown mesh draws comes from the engine's loaded set. That is often LOD 0 alone, because retail's unit LODs name no `MeshName`. At a few pixels a unit, this is not visible.
  - Each entity's reach is cached in a table indexed by entity id and read again on `SetMesh`. A minimap with nothing to draw costs a look per entity: about a tenth of the world view's gather on SCMP_009's 8,700 entities.
- **The draw.** The meshes go into the minimap's output after Stage 1, in their cartographic techniques. The output's pass now has a depth, the frame's own, cleared after Stage 0 is done with it. The view-projection is the frame's, with its top raised by 1000 so that aircraft are not clipped.
- **FAF.** FAF's patched game skips `RenderMeshes` while `cam_DefaultMiniLOD` is 0, which FAF's minimap toggle and `minimap.lua` set. The engine does the same when FAF's data is in use.

### 3.8 Main view cartographic (phase 7)

- `Camera` gains an ortho flag and the `UpdateCoords` ortho branch (1.0), fixed top-down, no shake; `WorldView::set_cartographic` on the main view sets it, with the prefs key and `ToggleMainCartographicView` working through retail's Lua unchanged.
- `record_main_pass` branches on the main WorldView's flag: it skips the sky, terrain, decals, water and shadow passes and draws the cartographic frame (window-sized), then the existing meshes/particles/icons/UI layers on top. `ui_AlwaysRenderStrategicIcons` is forced for the duration.
- Needs the zoom/aspect/clamp rules of `moho-camera-semantics` extended to ortho, and the cinematic restore of `gamemain.lua:546-571` to keep working.

**As built (P7, 2026-10-15).** Researched further in faf-re (`CameraImpl::UpdateCoords`, `WRenViewport::RenderCartographic`, `CWorldParticles::RenderEffects`, `RenderParticleBucket`):
- The ortho camera changes only UpdateCoords. The zoom, its glide and clamps, pan, and the heading and pitch (still found along for the focus) go on as in perspective. So `Camera::set_ortho` swaps the view (from straight above the focus, -z up), the projection, picking (straight down from over the pixel) and the shake (none).
- The projection's `r[3].x = -1/int(zoom)` and `r[3].y = +1/int(zoom/aspect)` are kept. They are a D3D9-style half-texel correction in world units, shifting the world by `width/(2·zoom)` pixels. Picking, the icons and the frame all go through the same matrix, so nothing misaligns. A view under one unit high takes one unit instead of dividing by zero.
- All the engine's world views share the renderer's camera, so only a main (non-minimap) view's `SetCartographic` switches it. A new main view starts in perspective, as a new `CameraImpl` does; retail's `Register` then restores the saved mode.
- The frame: Stage 0 and the overlays (the main view's range rings at its zoom, the fog's circles) into a window-sized frame of the main view's own, through the camera's matrix. Stage 1 draws straight into the scene pass, clamped as the 8-bit head clamps it, with alpha 0 (no glow).
- After Stage 1: the particles as `RenderEffects(camera, 0, 1)` draws them, which is the TLight ones alone (no beams, no trails), then the order lines, selection, resource and projectile icons and FAF's world drawing. The strategic icons are forced on (`set_forced`) without touching the player's convar.
- The shadow, normal and reflection passes still run (Moho draws the world for the view and then paints over it). Skipping them is an optimisation left for later.
- Tests: `--cartographic-main-test` (the scene is pixel for pixel the frame over the camera's rect, fogged with the army's circles; rings at the main view's zoom; icons; picking; toggling back and a new view), and `tests/test_camera_ortho.cpp`.

### 3.9 Left out at first (and why)

- **The playable boundary pass**: the engine has no BoundaryRenderer at all (`ren_PlayableBoundary`); not part of this design.
- **Fog**: until phase 5 the cartographic minimap is unfogged. The existing fog is a texture sampled by the terrain/unit shaders (`FogRenderer`, M215g); phase 5 adds a ground-quad darkening to 67% like the scene's, writing RGB only. Moho draws vision volumes (M215h is open).
- **Particles** on the cartographic frame (`RenderEffects(..., 0, 1, ...)`).
- **The non-cartographic minimap** (`SetCartographic(false)`): Moho renders the real world through a perspective top-down camera; the engine keeps its CPU gradient fallback. Out of scope.
- **Refresh after terrain flattening**: Moho builds once; so does this.
- **`mImagerElevationOffset`**: parsed, unused (belongs to `MapImager`).
- **Multi-head / split screen**: one frame per view.

## 4. Phased plan

Each phase is one reviewable PR, with its own tests, in the repo's patterns (unit tests in `tests/` listed in `tests/CMakeLists.txt`, `osc_integration` modes registered in `test_modes.cpp` / `integration_tests.hpp` / `data_tests.cmake` / `tests/integration/CMakeLists.txt`, render probes like `sky_test.cpp`'s).

**P1. Data and pure math** (no GPU)
- Parser: tileset block and decal batches; `Terrain` accessors (with the deep water's start); `cartographic_math`.
- Unit tests:
  - `test_cartographic_math`: coefficients for 512 and 2048 maps; elevation texel `(x,z)` against the formula (the shift and clamp); the half-float conversion (exact values, denormals, saturation); the hypsometric ramp for water/no-water (the boundary texels at `nD/nW/nS`); `blend_packed_color` (truncation, all four bytes); the topographic ramp (bands 1, 20, 100; water zero; band clamp at the top); the ortho matrix against the closed form.
  - `test_map` additions: a synthetic v54/v56/v57/v59/v60 buffer reads the right samples and colours; v < 56 gets Moho's defaults; the batches parse and the props still follow.
- Integration `--cartographic-test` (new file `tests/integration/runner/cartographic_test.cpp`, mode table entry, `cartographic-test` in `OSC_DATA_TESTS_GATE`): part 1 pins SCMP_009's samples and five colours (values dumped from the file once and written into the test, as the sky test pins its block) and surveys all retail maps: parse OK, props found, batch counts printed (this decides whether P4 is worth doing).
- No goldens.

**P2. Terrain on the GPU** (not wired to the UI)
- *As built:* both targets are `R8G8B8A8_UNORM`, not HDR. Moho's are 8-bit, and the edge detect works on the 8-bit band shades, so the port rounds where Moho's did. P5 makes the overlay pipelines for this pass. The test hook is `Renderer::capture_cartographic(terrain, view)`, which draws in a command buffer of its own and reads both stages back. On SCMP_009 every pixel matches the CPU reference, square and letterboxed, with no validation errors. The `±0.0001` and `16 → 8` mutations fail it.
- `CartographicRenderer` (terrain + edge pipelines, textures, targets), shaders, `upload_pixels`, and a test hook `Renderer::request_cartographic_capture(cb)` (the frame or the output, as floats, like `request_scene_capture`, `renderer.hpp:424-435`) with a probe view (size and map rect) so the frame renders with no UI.
- `--cartographic-test` part 2 on SCMP_009 at 512x512 and 640x384 (letterbox):
  - Stage0: every pixel against a test-side CPU reference of `TerrainPS0` fed from the pure-math textures (colour and alpha);
  - output: the whole frame against the CPU reference of `TerrainPS1`;
  - structure: the shore line is darker than the land beside it; no contour line on the water; alpha 0 everywhere in the output; black outside the map in the letterbox.
- Mutation list (all must be killed): `+0.0001` for `-`; the `-1` sample shift; the `floor(h*256)` index as `h*255`; the ramp's `n <= nD` order; the kernel; neighbours (one dropped); `16` for `8`; edge on the lower side; unsaturated output; alpha written as 1; the contour ramp's 16 bias; depth write off (checked through a range overlay in P5); band clamp.
- No goldens.

**P3. The minimap shows it**
- `MinimapRenderer` and `renderer.cpp` wiring (3.6); the flag honoured per view; the CPU texture kept as the fallback; `make_descriptor` with a point sampler; target sizing and recreation; the HUD path.
- Tests: `--cartographic-test` part 3 with the FA UI loaded (the existing `--prefs fixtures/minimap.prefs` fixture): the minimap view's flag is true after retail's `minimap.lua`; the painted quad's texture is the cartographic target; with `SetCartographic(false)` the old texture returns; a click still maps to the world (`hit_test` unchanged); the icons are still painted; a window resize recreates the target without a validation error. Unit test for the target size snapping.
- **Goldens**: `data.golden.scmp009_game_ui_minimap` changes by design. It needs your review, as a rebaseline: record with the test's args plus `--golden-update` into `~/.local/state/opensupcom/golden`, and quote the changed-pixel percentage next to the image (the percentage must be in the PR; a "pre-existing" failure would hide it). `scmp009_game_ui` (no minimap) must not change.

**P4. Cartographic decals** (P1's survey: no retail map has a batch, so this serves user maps only; deferred)
- Decal pipelines, instance buffer (1024 per draw; chunked beyond), texture resolve through the cache, `Decal` and `DecalGlow` by technique name.
- Tests: a synthetic batch injected through `Terrain` (the test can set batches), pixel checks of the albedo against `DecalPS` at DecalVS's UV (as the sky decal test does), the glow pass writing alpha only, rotation and the v flip, a technique name unknown -> not drawn.

**P5. Overlays in the frame**
- Range overlays (the existing `RangeRenderer::render(cmd, w, h, view_proj, fi)` with the minimap's matrix; ring thickness scaled for the minimap), fog (a ground-quad pass), drawn in pass A so they go through the edge detect as in Moho.
- Check render-pass compatibility between the cartographic passes and the scene pass first; if not compatible, the pipelines are created a second time for the new pass.
- Tests: selected-unit rings appear in the minimap frame at the right place (probe at the ring's radius); fogged cells are 67% of unfogged; ring depth-test against the flat quad (the ZWrite that P2's mutation list defers).
- Risk: the rings' alpha writes would distort the contour alpha; see risks.
- *As built ([#670](https://github.com/bwiemz/supcom-engine/pull/670)):* the render passes aren't compatible (the frame is 8-bit, the scene HDR), so the minimap has a second `RangeRenderer` made for the frame's pass. The stencil-volume code both share moved to `stencil_volume.{hpp,cpp}`.
  - **Rings:** `collect_range_scene` runs a second time with a frustum of the frame's view (the whole map, as the minimap camera's is). The zoom ratio is 1: minimap.lua's `CameraReset` then `SetMaxZoomMult(1.0)` leaves the camera at its largest zoom, so the lines take their zoomed-out thickness. FAF's toggle for the minimap's rings (`hooks/Minimap.hook`) isn't ported.
  - **Fog** is Moho's `RenderFogOfWar`, not a ground-quad darkening. `VisionRenderer` casts each vision circle as a cylinder of the ring volume (inner radius 0) into the stencil against the quad's depth: z-fail, front faces `incr`, back faces `decr`, wrapping as vision.fx's states do, so 256 circles over one point count it back to fogged. frame.fx's `Vision` then darkens what counted 0 with black at alpha 0.33, colour only, so the contour alpha is untouched.
  - **The circles** follow `UserUnit::Tick`'s VisionDB handle: every unit with a vision range whose army the focus army calls an ally (its own included), at its interpolated position, its radius the range's whole units. A unit being built or dying has none, and the vision toggle doesn't matter. Not drawn for an observer, with the engine's fog off, or with no fog of war.
  - **Left out:** script entities' vision (VizMarker) and `CreateVisibleAreaAtPoint`. Which army a VizMarker's user entity has isn't known (its spec names no owner), and retail's scripts never call the other.
  - **Order** as `Cartographic::Render`'s: Stage 0, the rings, the fog, then Stage 1. Fogged rings dim with the ground, and the rings' glow alpha goes through the edge detect as Moho's does.
  - **The frame's depth range** takes in the volumes (5 below the map's lowest point to its highest), or the cylinders' bottom caps, the only faces behind the quad seen from above, would be clipped. The volumes themselves keep Moho's span (`VisionRenderer::Init` and the range volumes read the map's height range), so on a map flooded over its highest ground the quad lies above them and nothing counts, as it would in Moho.
  - **Tests:** `--cartographic-test` checks the fog's pixels (clear in the circle, 67% outside with alpha unchanged), no vision, the 255/256 wrap, a ring's line, fill and beyond, and the order. `--cartographic-minimap-test` checks a live frame's circles, the selected unit's ring at full zoom, and an observer's no fog. Mutations killed: the back faces' count clamped, the fog's alpha, its alpha write, fog before rings, no depth widening, Stage 0's depth write off (P2's deferred one), the world view's zoom ratio. The front faces' count clamped survives, and can't be seen here: from above, the only faces behind the quad are the cylinders' bottom caps, all one facing.

**P6. Meshes** — only after `mesh.fx` is extracted and read (3.7). Tests: the draw counts of materials with and without the annotation; depth ordering; probes of a unit's colour against the ramp.

**P7. Main view** (3.8) — camera ortho path with unit tests (matrix, fixed look, no shake), the pass switch, prefs and hotkey, cinematic restore. Integration: toggling `SetCartographic` on the main view changes the scene (no sky, no water; the cartographic frame at window size); goldens for the main view in cartographic mode (new, with your review).

**P8. Retail A/B** — a screenshot of retail's minimap and cartographic main view on SCMP_009 (a manual screenshot, or the Proton probe route from the notes), compared with the engine's; settles risks 1-3. Not a PR on its own unless it changes constants.

## 5. Tests overview

| Layer | What | Where |
|---|---|---|
| Unit | ramps, textures, half-float, ortho, parser | `tests/test_cartographic_math.cpp`, `tests/test_map*.cpp` (new files listed in `tests/CMakeLists.txt` near `test_sky_dome.cpp:130`) |
| Integration gate | `--cartographic-test` parts 1-3 | `tests/integration/runner/cartographic_test.cpp`, `test_modes.cpp` table (next to `{"--sky-test", test_sky, false}` at line 171), `integration_tests.hpp` (decl near 160), `data_tests.cmake:52`, `tests/integration/CMakeLists.txt` (runner list near 58) |
| Render probes | whole-frame CPU reference compare, structure checks | `Renderer::capture_cartographic` |
| Golden | the minimap rebaseline (P3), the main view (P7) | machine-local PNGs; changes need your review |

## 6. Risks and open questions

1. **Default/water elevation (doubt 1).** The decompile uses the deep elevation for both lanes. If the binary uses the abyss for the third, the water has three blues, not two. The port follows the decompile behind one named constant; P8 settles it from a screenshot of a map with an abyss. P1's survey found [1] grey (`0xFF707070`) on 53 of 60 retail maps, which makes a [1] water band unlikely and supports the decompile.
2. **Kernel and tier box.** If the kernel really smooths over 3x3, the elevation map differs slightly (soft contour lines); if the tier box is not the exact min/max, every colour band shifts. P8 compares contour positions.
3. **Alpha under overlays.** Range rings and fog are drawn in the frame whose alpha carries the contour value. If Moho's overlay pipelines write alpha, the edge detect sees rings. Unknown; the port's overlay passes write RGB only, which keeps contours clean.
4. **Mesh techniques are unknown**, and the engine has no cartographic mesh code. Blocks P6. Needs `mesh.fx` from the retail `effects` archive.
5. **Resampling.** The minimap rect is a float; a 1-pixel contour line blurs if the target is stretched. Mitigated by integer snapping and a point sampler; check at UI scales and on window resize.
6. **Frame-in-flight hazards** of a render target read by the UI in the same command buffer; per-fi images avoid them.
7. **Render-pass compatibility** with the scene's pipelines must hold for P5/P6; if the scene pass changes (water passes, M213), the cartographic passes must be rebuilt with it.
8. **Main-view camera.** The engine's `Camera` has no ortho path; the decompile's `r[3]` terms are suspect; the zoom/clamp rules for ortho are unresearched. P7 is the largest and least certain phase.
9. **Retail has no cartographic batches** (P1's survey of all 60 maps): P4 is for user maps only.
10. **Flattening.** The elevation map is built once, as Moho builds it; the P1 terrain-integration roadmap item (grid refresh after flatten) may want a hook.
11. **Version gates** of the tileset block: settled in P1 as 56/57, by faf-re and the FAF map editor.
12. **Goldens.** The minimap golden changes in P3; every later phase adds or moves goldens. All need Brandon's review; none is checked in (they live outside the repo).
13. **Decompile ABI caveats.** `RenderTerrainStage0` takes a `mirrored` bool that is never read; `Initialize` takes five colours that are re-read from the terrain; both are dropped.

Open questions for Brandon:
- Is the CPU-gradient minimap to be deleted once P3 lands, or kept for `SetCartographic(false)`? (This design keeps it as the fallback.)
- Is the main-view cartographic (P7) in scope now, or after the minimap ships?
- Can you take a reference screenshot of retail's minimap and cartographic main view on SCMP_009 (P8)? The Proton probe route in the notes reads Lua state, not pixels.
