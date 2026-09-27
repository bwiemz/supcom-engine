#pragma once

#include "core/types.hpp"

#include <optional>
#include <string_view>
#include <vector>

namespace osc {

/// A colour as Moho's SCR_DecodeColor reads one: a colour name (the 140 web
/// colours and "transparent", in any case), or exactly 6 or 8 hex digits
/// (6 are opaque). Packed ARGB; nothing for anything else.
std::optional<u32> decode_color(std::string_view text);

/// The colour names, in Moho's order (what EnumColorNames returns).
const std::vector<std::string_view>& color_names();

} // namespace osc
