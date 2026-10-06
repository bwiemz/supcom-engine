#pragma once

#include "core/types.hpp"

#include <optional>
#include <vector>

namespace osc::ui {

/// Which texels of a DDS file's top mip have a nonzero alpha, a bit each.
class AlphaMask {
public:
    static std::optional<AlphaMask> from_dds(const std::vector<char>& file);

    bool opaque(i64 x, i64 y) const;

private:
    u32 width_ = 0;
    u32 height_ = 0;
    std::vector<bool> bits_;
};

} // namespace osc::ui
