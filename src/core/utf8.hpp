#pragma once

#include "core/types.hpp"

#include <string_view>

namespace osc {

/// The character at byte `i` of UTF-8 text, moving `i` past it; a byte that
/// starts no well-formed sequence is taken as itself (Latin-1).
u32 next_codepoint(std::string_view text, size_t& i);

} // namespace osc
