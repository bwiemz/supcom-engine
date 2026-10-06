#pragma once

#include "core/types.hpp"

#include <optional>
#include <vector>

namespace osc::renderer {

/// Decode a single BC3 (DXT5) 4x4 block to 16 RGBA pixels (64 bytes output).
void decode_bc3_block(const u8* block, u8* out_rgba);

/// Decode a full BC3-compressed mip level to RGBA pixels.
/// Returns width*height*4 bytes of RGBA pixel data.
std::vector<u8> decode_bc3_to_rgba(const u8* block_data, u32 width, u32 height);

/// A DDS file's top mip level as RGBA bytes, rows top down (as GLFW takes a
/// cursor's image), its size in `width` and `height`: from uncompressed
/// 32-bit (either channel order) or BC3. Nothing for any other format.
std::optional<std::vector<u8>> dds_to_rgba(const std::vector<char>& file, u32& width, u32& height);

} // namespace osc::renderer
