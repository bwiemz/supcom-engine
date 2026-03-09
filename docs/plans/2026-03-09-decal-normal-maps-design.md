# M128: Decal Normal Map Rendering — Design

## Goal

Render normal map decals (decal_type == 2) from .scmap files so they perturb terrain normals and affect lighting, instead of being skipped entirely.

## Architecture

CPU-baked normal overlay texture, created once at map load. Normal map decals are rasterized into an RG16F texture in world-space XZ coordinates, then uploaded to the GPU. The terrain fragment shader samples this overlay to perturb the heightmap normal before lighting is computed.

### Why CPU-baked?

FA's map decals are static (placed by the map editor, never change at runtime). Baking at load time is simplest and has zero per-frame rendering cost. Runtime decals from Lua `CreateDecal` are a separate system that can be added later.

## Data Flow

1. **scenario_loader.cpp** — Remove the `if (d.decal_type == 2) continue;` filter. Separate decals into two lists: albedo (type 1, existing pipeline) and normal (type 2, new baking path). Pass normal decals to terrain or renderer.

2. **Normal overlay baking** (new code in renderer) — For each normal map decal:
   - Load the DDS normal map texture via VFS
   - Decode to CPU RGBA pixels (DDS → raw)
   - Project the decal's world-space position/rotation/scale onto the overlay's XZ grid
   - For each covered texel, sample the decal's tangent-space normal, transform by decal rotation, and alpha-blend into the overlay

3. **Overlay texture** — RG16F format (X and Y perturbation; Z reconstructed in shader via `sqrt(1 - x² - y²)`). Resolution = map width in game units (1 texel per game unit). Initialized to (0, 0) meaning "no perturbation". Uploaded once.

4. **Terrain shader** — Add normal overlay as a second texture binding. Sample at world XZ / map_size UV. If overlay != (0,0), perturb the terrain normal using the overlay's XY as tangent-space offsets. Use perturbed normal for all lighting (diffuse, specular, hemisphere ambient).

## Tangent Space

Terrain tangent basis: tangent = +X, bitangent = +Z, normal = +Y. The overlay stores perturbations in this frame. Decal rotation transforms the decal's local tangent space into terrain tangent space before writing to the overlay.

## Blending

Where multiple normal decals overlap, normals are alpha-averaged. Approximate but sufficient for static map decals.

## Resolution

1 texel per game unit. A 10km map (map_size = 512) gets a 512x512 overlay (~1MB). A 20km map gets 1024x1024 (~4MB). Scales naturally with map size.

## Files Changed

| File | Change |
|------|--------|
| `src/lua/scenario_loader.cpp` | Remove type-2 filter, pass normal decals to renderer |
| `src/map/terrain.hpp` | Add `NormalDecalInfo` struct with decal_type + texture path |
| `src/renderer/renderer.hpp` | Add normal_overlay_ VkImage/view/sampler, bake function decl |
| `src/renderer/renderer.cpp` | Bake overlay in build_scene(), bind to terrain pipeline |
| `src/renderer/shader_utils.cpp` | Update terrain fragment shader to sample + perturb normals |
| `src/renderer/texture_cache.hpp/cpp` | Add CPU-side DDS decode helper if needed |

## Performance

- **Load time**: One-time bake cost (iterate decals, decode DDS, rasterize). Negligible for typical map decal counts (50-200).
- **Per-frame**: One extra texture sample per terrain fragment. No measurable impact.
- **Memory**: 1-4 MB for the overlay texture.

## Testing

- Unit test: CPU baking logic (rasterize a synthetic normal decal onto an overlay buffer, verify perturbed values)
- Visual test: Load a map with normal decals, verify terrain shows surface detail (cracks, craters) that responds to light direction
