# M169: Map Preview in Lobby — Design Spec

**Goal:** FA-compatible MapPreview moho control that displays map preview images in the skirmish lobby. FA's lobby code (`MapPreview(parent)`, `SetTexture`, `SetTextureFromMap`, `ClearTexture`) runs unmodified.

---

## 1. Architecture

Three changes work together:

1. **SCMAP preview extraction** — Extend `ScmapParser` to store the embedded DDS preview blob instead of skipping it. Add `std::vector<char> preview_dds` to `ScmapData` (matching existing `blend_dds_0`/`blend_dds_1` type).
2. **MapPreview moho control** — New control type with `SetTexture(path)` (loads DDS from VFS), `SetTextureFromMap(scmap_path)` (extracts SCMAP preview DDS, or generates heightmap-based preview as fallback), and `ClearTexture()` (blanks the preview). All upload to TextureCache and render as a textured quad via the existing UI pipeline.
3. **Heightmap fallback** — When no preview DDS exists in the SCMAP or VFS, generate a terrain preview from the heightmap using a shared utility function at 512x512 resolution.

---

## 2. SCMAP Preview Extraction

The parser at `scmap_parser.cpp:389-395` currently reads the preview length then calls `r.skip()`. Change to store the raw DDS blob using the existing `BinaryReader::read_bytes()` pattern (same as `blend_dds_0` at line 287):

```cpp
// Store preview DDS data instead of skipping
scmap.preview_dds = r.read_bytes(static_cast<size_t>(preview_length));
```

Add to `ScmapData` struct in `scmap_parser.hpp`:
```cpp
std::vector<char> preview_dds;  // Raw DDS blob from SCMAP (may be empty)
```

**Type choice:** `std::vector<char>` — matching `blend_dds_0`/`blend_dds_1` and compatible with `parse_dds()` and `TextureCache::get_raw()` which both take `const std::vector<char>&`.

---

## 3. MapPreview Moho Control

### Lua API

Matches FA's `mappreview.lua` (`ClassUI(moho.ui_map_preview_methods, Control)`):
```lua
local preview = MapPreview(parent)
local ok = preview:SetTexture('/maps/SCMP_009/lobby/preview.dds')  -- returns bool
if not ok then
    preview:SetTextureFromMap('/maps/SCMP_009/SCMP_009.scmap')     -- fallback
end
preview:ClearTexture()  -- blanks the preview (used when switching maps)
```

### C++ Implementation

**InternalCreateMapPreview(self, parent):**
- Creates a standard UIControl with the 7 layout LazyVars (Left/Top/Right/Bottom/Width/Height/Depth)
- Same pattern as `InternalCreateBitmap` (moho_bindings.cpp:10009-10062)
- Registered as global function for FA's `MapPreview(parent)` constructor

**mappreview_SetTexture(path) → bool:**
- Loads DDS from VFS via `TextureCache::get_blocking(path)` — **must be synchronous** because FA's lobby code uses the return value to decide whether to fall back to `SetTextureFromMap`. The async `get()` would always return nullptr on first call.
- If successful, sets the texture key on the control (`ctrl->set_texture_path(path)`)
- Returns true if texture loaded, false otherwise

**mappreview_SetTextureFromMap(scmap_path):**
1. Read SCMAP file from VFS
2. Parse with `ScmapParser` to get `ScmapData` (now includes `preview_dds`)
3. If `preview_dds` is non-empty: upload compressed DDS directly via `TextureCache::get_raw(key, preview_dds)` with key `"__osc_mappreview_<scmap_path>"`. This preserves the original DXT compression on GPU (32KB DXT1 vs 256KB RGBA).
4. If `preview_dds` is empty or `get_raw` fails: generate heightmap-based preview (Section 4) and upload via `TextureCache::upload_rgba()`
5. Set `ctrl->set_texture_path("__osc_mappreview_<scmap_path>")` so the UIRenderer finds the texture

**mappreview_ClearTexture():**
- Clears `ctrl->set_texture_path("")` — UIRenderer falls back to no visual, effectively blanking the preview
- Called by `ResourceMapPreview:Clear()` and `SetScenario()` when switching maps

### Method Table

Registered under `ui_map_preview_methods` in the `moho` table (matching FA's `ClassUI(moho.ui_map_preview_methods, Control)`):

```cpp
static const MethodEntry ui_map_preview_methods[] = {
    {"SetTexture",        mappreview_SetTexture},
    {"SetTextureFromMap", mappreview_SetTextureFromMap},
    {"ClearTexture",      mappreview_ClearTexture},
    {nullptr, nullptr},
};
```

### Rendering

The UIRenderer already handles textured controls: `texture_path_` → `TextureCache::get()` → `descriptor_set` → textured quad. MapPreview uses this exact path. After uploading the preview texture with a synthetic key, the UIRenderer resolves it like any other bitmap texture.

---

## 4. Heightmap Fallback

When the SCMAP has no embedded preview DDS, generate a terrain preview from the heightmap data in the SCMAP file.

### Shared Utility Function

Extract the terrain color algorithm from `MinimapRenderer::build_terrain_texture()` (minimap_renderer.cpp:79-151) into a shared function:

```cpp
// In src/renderer/terrain_preview.hpp
namespace osc::renderer {
    /// Generate RGBA terrain preview from raw heightmap data.
    /// heightmap array is (map_width+1) * (map_height+1) samples (grid vertices).
    /// map_width/map_height are in game units (the heightmap grid is +1 in each dimension).
    /// Returns output_size * output_size * 4 bytes of RGBA pixel data.
    std::vector<u8> generate_terrain_preview(
        const u16* heightmap, u32 map_width, u32 map_height,
        f32 height_scale, f32 water_elevation, bool has_water,
        u32 output_size = 512);
}
```

**Heightmap indexing:** The heightmap grid is `(map_width+1) × (map_height+1)` samples. The function internally uses `grid_w = map_width + 1` for array indexing to avoid out-of-bounds access.

**Algorithm** (same as minimap):
- Sample heightmap at grid positions to fill `output_size × output_size` pixels
- Water pixels: blue tones, depth-darkened
- Land pixels: height-based gradient (dark green → bright green → brown → grey)
- The `ScmapData` struct already contains `heightmap`, `map_width`, `map_height`, `height_scale`, and `water_elevation`

**MinimapRenderer refactored** to call the shared function instead of inlining the algorithm.

---

## 5. File Layout

```
New files:
  src/renderer/terrain_preview.hpp   — generate_terrain_preview() declaration
  src/renderer/terrain_preview.cpp   — shared terrain preview generation

Modified files:
  src/map/scmap_parser.hpp           — Add preview_dds field to ScmapData
  src/map/scmap_parser.cpp           — Store preview DDS via read_bytes() instead of skip
  src/renderer/minimap_renderer.cpp  — Refactor to use shared terrain_preview
  src/lua/moho_bindings.cpp          — MapPreview control + methods (ui_map_preview_methods)
```

---

## 6. What This Does NOT Cover

- **Resource marker icons** — FA's `ResourceMapPreview` overlays mass/energy marker icons on top of the map preview. This is pure Lua (positions calculated from marker data). It will work automatically once MapPreview renders the base image.
- **Water/cliff/buildable overlays** — FA has optional overlay DDS files. These are secondary — the base preview is sufficient for M169.
- **Live 3D preview** — Some mods render a rotating 3D preview. Out of scope.

---

## 7. Testing

- `--mappreview-test` flag: Creates a MapPreview control, calls `SetTextureFromMap` with the test map's SCMAP, verifies:
  - SCMAP preview DDS extracted (non-empty `preview_dds` field in ScmapData)
  - Control has non-empty texture_path after SetTextureFromMap
  - ClearTexture sets texture_path to empty
- Unit test: `generate_terrain_preview()` with synthetic heightmap data, verify output dimensions and non-zero pixels
- Fallback test: SCMAP with empty preview → heightmap fallback generates valid RGBA data
- Note: TextureCache GPU-side verification only works with Vulkan device (not headless)
