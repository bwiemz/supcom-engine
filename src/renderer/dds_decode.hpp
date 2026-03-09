#pragma once

#include "core/types.hpp"

#include <vector>

namespace osc::renderer {

/// Decode a single BC3 (DXT5) 4x4 block to 16 RGBA pixels (64 bytes output).
void decode_bc3_block(const u8* block, u8* out_rgba);

/// Decode a full BC3-compressed mip level to RGBA pixels.
/// Returns width*height*4 bytes of RGBA pixel data.
std::vector<u8> decode_bc3_to_rgba(const u8* block_data, u32 width, u32 height);

} // namespace osc::renderer
