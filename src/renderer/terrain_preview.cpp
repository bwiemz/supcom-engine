#include "renderer/terrain_preview.hpp"

#include <algorithm>
#include <cmath>

namespace osc::renderer {

std::vector<u8> generate_terrain_preview(
    const u16* heightmap, u32 map_width, u32 map_height,
    f32 height_scale, f32 water_elevation, bool has_water,
    u32 output_size) {

    std::vector<u8> pixels(output_size * output_size * 4, 0);
    if (!heightmap || map_width == 0 || map_height == 0) return pixels;

    u32 grid_w = map_width + 1;
    u32 grid_h = map_height + 1;

    // Find height range for normalization
    f32 min_h = 1e9f, max_h = -1e9f;
    for (u32 gz = 0; gz < grid_h; gz += 4) {
        for (u32 gx = 0; gx < grid_w; gx += 4) {
            f32 h = static_cast<f32>(heightmap[gz * grid_w + gx]) * height_scale;
            min_h = std::min(min_h, h);
            max_h = std::max(max_h, h);
        }
    }
    if (max_h - min_h < 1.0f) max_h = min_h + 1.0f;

    for (u32 ty = 0; ty < output_size; ty++) {
        for (u32 tx = 0; tx < output_size; tx++) {
            // Map pixel to grid position
            f32 gx_f = (static_cast<f32>(tx) + 0.5f) / output_size * (grid_w - 1);
            f32 gz_f = (static_cast<f32>(ty) + 0.5f) / output_size * (grid_h - 1);
            u32 gx = std::min(static_cast<u32>(gx_f), grid_w - 1);
            u32 gz = std::min(static_cast<u32>(gz_f), grid_h - 1);
            f32 h = static_cast<f32>(heightmap[gz * grid_w + gx]) * height_scale;

            u32 idx = (ty * output_size + tx) * 4;

            if (has_water && h < water_elevation) {
                f32 depth_frac = std::clamp(
                    (water_elevation - h) / (water_elevation - min_h + 1.0f), 0.0f, 1.0f);
                pixels[idx + 0] = static_cast<u8>(20 + (1.0f - depth_frac) * 40);
                pixels[idx + 1] = static_cast<u8>(40 + (1.0f - depth_frac) * 60);
                pixels[idx + 2] = static_cast<u8>(80 + (1.0f - depth_frac) * 80);
            } else {
                f32 t = std::clamp((h - min_h) / (max_h - min_h), 0.0f, 1.0f);
                if (t < 0.4f) {
                    f32 s = t / 0.4f;
                    pixels[idx + 0] = static_cast<u8>(30 + s * 30);
                    pixels[idx + 1] = static_cast<u8>(50 + s * 50);
                    pixels[idx + 2] = static_cast<u8>(20 + s * 10);
                } else if (t < 0.7f) {
                    f32 s = (t - 0.4f) / 0.3f;
                    pixels[idx + 0] = static_cast<u8>(60 + s * 60);
                    pixels[idx + 1] = static_cast<u8>(100 - s * 30);
                    pixels[idx + 2] = static_cast<u8>(30 + s * 20);
                } else {
                    f32 s = (t - 0.7f) / 0.3f;
                    pixels[idx + 0] = static_cast<u8>(120 + s * 40);
                    pixels[idx + 1] = static_cast<u8>(70 + s * 50);
                    pixels[idx + 2] = static_cast<u8>(50 + s * 40);
                }
            }
            pixels[idx + 3] = 255;
        }
    }

    return pixels;
}

} // namespace osc::renderer
