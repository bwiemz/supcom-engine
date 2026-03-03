#pragma once

#include "core/types.hpp"

#include <vulkan/vulkan.h>

#include <optional>
#include <vector>

namespace osc::renderer {

/// One mip level within a DDS file.  data points into the source file buffer.
struct DDSMipLevel {
    const char* data;   // not owned — points into the file_data vector
    u32 width;
    u32 height;
    u32 size;           // byte size of this mip level's compressed data
};

/// Parsed DDS file metadata + mip-level pointers.
struct DDSTexture {
    VkFormat format = VK_FORMAT_UNDEFINED;
    u32 width = 0;
    u32 height = 0;
    u32 mip_count = 0;
    std::vector<DDSMipLevel> mips;
};

/// Parse a DDS file (BC1/BC2/BC3 compressed).
/// The returned DDSMipLevel::data pointers reference bytes in file_data,
/// so file_data must outlive the DDSTexture.
/// Returns nullopt on failure (bad magic, unsupported format, truncated).
std::optional<DDSTexture> parse_dds(const std::vector<char>& file_data);

} // namespace osc::renderer
