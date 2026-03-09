#include "renderer/dds_decode.hpp"

#include <cstring>

namespace osc::renderer {

// ---------------------------------------------------------------------------
// Alpha block decoding (first 8 bytes of a BC3 block)
// ---------------------------------------------------------------------------

static void decode_alpha_block(const u8* src, u8 alpha_out[16]) {
    const u8 a0 = src[0];
    const u8 a1 = src[1];

    // Build 8-entry interpolation table
    u8 table[8];
    table[0] = a0;
    table[1] = a1;
    if (a0 > a1) {
        table[2] = static_cast<u8>((6 * a0 + 1 * a1) / 7);
        table[3] = static_cast<u8>((5 * a0 + 2 * a1) / 7);
        table[4] = static_cast<u8>((4 * a0 + 3 * a1) / 7);
        table[5] = static_cast<u8>((3 * a0 + 4 * a1) / 7);
        table[6] = static_cast<u8>((2 * a0 + 5 * a1) / 7);
        table[7] = static_cast<u8>((1 * a0 + 6 * a1) / 7);
    } else {
        table[2] = static_cast<u8>((4 * a0 + 1 * a1) / 5);
        table[3] = static_cast<u8>((3 * a0 + 2 * a1) / 5);
        table[4] = static_cast<u8>((2 * a0 + 3 * a1) / 5);
        table[5] = static_cast<u8>((1 * a0 + 4 * a1) / 5);
        table[6] = 0;
        table[7] = 255;
    }

    // 6 bytes of 3-bit indices (48 bits = 16 pixels)
    // Packed as 3 groups of 2 bytes = 3 groups of 8 pixels? No:
    // 48 bits packed across 6 bytes, 3 bits per pixel, row by row.
    // Bytes 2..7 contain the indices. Each group of 3 bytes holds 8 indices.
    const u8* idx_bytes = src + 2;
    for (u32 group = 0; group < 2; ++group) {
        u32 packed = static_cast<u32>(idx_bytes[group * 3 + 0])
                   | (static_cast<u32>(idx_bytes[group * 3 + 1]) << 8)
                   | (static_cast<u32>(idx_bytes[group * 3 + 2]) << 16);
        for (u32 i = 0; i < 8; ++i) {
            u32 idx = (packed >> (3 * i)) & 0x7;
            alpha_out[group * 8 + i] = table[idx];
        }
    }
}

// ---------------------------------------------------------------------------
// Color block decoding (last 8 bytes of a BC3 block)
// ---------------------------------------------------------------------------

static void decode_color_block(const u8* src, u8 rgb_out[16 * 3]) {
    // Two RGB565 endpoints
    u16 c0_raw = static_cast<u16>(src[0]) | (static_cast<u16>(src[1]) << 8);
    u16 c1_raw = static_cast<u16>(src[2]) | (static_cast<u16>(src[3]) << 8);

    // Expand RGB565 to RGB888
    auto expand565 = [](u16 c, u8& r, u8& g, u8& b) {
        u8 r5 = static_cast<u8>((c >> 11) & 0x1F);
        u8 g6 = static_cast<u8>((c >> 5) & 0x3F);
        u8 b5 = static_cast<u8>(c & 0x1F);
        r = static_cast<u8>((r5 << 3) | (r5 >> 2));
        g = static_cast<u8>((g6 << 2) | (g6 >> 4));
        b = static_cast<u8>((b5 << 3) | (b5 >> 2));
    };

    u8 colors[4][3];
    expand565(c0_raw, colors[0][0], colors[0][1], colors[0][2]);
    expand565(c1_raw, colors[1][0], colors[1][1], colors[1][2]);

    if (c0_raw > c1_raw) {
        // 4-color mode: 2 interpolated
        for (u32 ch = 0; ch < 3; ++ch) {
            colors[2][ch] = static_cast<u8>((2 * colors[0][ch] + colors[1][ch]) / 3);
            colors[3][ch] = static_cast<u8>((colors[0][ch] + 2 * colors[1][ch]) / 3);
        }
    } else {
        // 3-color + transparent mode
        for (u32 ch = 0; ch < 3; ++ch) {
            colors[2][ch] = static_cast<u8>((colors[0][ch] + colors[1][ch]) / 2);
            colors[3][ch] = 0;
        }
    }

    // 4 bytes of 2-bit indices (16 pixels)
    const u8* idx_bytes = src + 4;
    for (u32 row = 0; row < 4; ++row) {
        u8 bits = idx_bytes[row];
        for (u32 col = 0; col < 4; ++col) {
            u32 idx = (bits >> (2 * col)) & 0x3;
            u32 pixel = row * 4 + col;
            rgb_out[pixel * 3 + 0] = colors[idx][0];
            rgb_out[pixel * 3 + 1] = colors[idx][1];
            rgb_out[pixel * 3 + 2] = colors[idx][2];
        }
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void decode_bc3_block(const u8* block, u8* out_rgba) {
    // BC3 block = 8 bytes alpha + 8 bytes color = 16 bytes total
    u8 alpha[16];
    u8 rgb[16 * 3];

    decode_alpha_block(block, alpha);
    decode_color_block(block + 8, rgb);

    // Interleave into RGBA
    for (u32 i = 0; i < 16; ++i) {
        out_rgba[i * 4 + 0] = rgb[i * 3 + 0]; // R
        out_rgba[i * 4 + 1] = rgb[i * 3 + 1]; // G
        out_rgba[i * 4 + 2] = rgb[i * 3 + 2]; // B
        out_rgba[i * 4 + 3] = alpha[i];        // A
    }
}

std::vector<u8> decode_bc3_to_rgba(const u8* block_data, u32 width, u32 height) {
    // BC3 blocks are 4x4 pixels, dimensions rounded up to multiples of 4
    const u32 bw = (width + 3) / 4;   // blocks wide
    const u32 bh = (height + 3) / 4;  // blocks tall

    std::vector<u8> pixels(static_cast<size_t>(width) * height * 4, 0);

    for (u32 by = 0; by < bh; ++by) {
        for (u32 bx = 0; bx < bw; ++bx) {
            const u8* block = block_data + (by * bw + bx) * 16;

            u8 decoded[64]; // 16 pixels * 4 bytes
            decode_bc3_block(block, decoded);

            // Copy decoded pixels into the output image, clamping at edges
            for (u32 py = 0; py < 4; ++py) {
                const u32 y = by * 4 + py;
                if (y >= height) break;
                for (u32 px = 0; px < 4; ++px) {
                    const u32 x = bx * 4 + px;
                    if (x >= width) break;
                    const u32 src_offset = (py * 4 + px) * 4;
                    const u32 dst_offset = (y * width + x) * 4;
                    std::memcpy(&pixels[dst_offset], &decoded[src_offset], 4);
                }
            }
        }
    }

    return pixels;
}

} // namespace osc::renderer
