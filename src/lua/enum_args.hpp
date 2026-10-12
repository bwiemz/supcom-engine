#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <string_view>

namespace osc::lua {

/// A name of Moho's that Lua gives in any case, with or without its prefix
/// (gpg's REnumType::SetLexical, as SCR_GetEnum reads one); its index in
/// `names`, or -1.
template <std::size_t N>
int parse_enum(std::string_view text, std::string_view prefix,
               const std::array<std::string_view, N>& names) {
    const auto same = [](std::string_view a, std::string_view b) {
        return a.size() == b.size() &&
               std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
                   return std::tolower(static_cast<unsigned char>(x)) ==
                          std::tolower(static_cast<unsigned char>(y));
               });
    };
    if (text.size() > prefix.size() && same(text.substr(0, prefix.size()), prefix))
        text.remove_prefix(prefix.size());
    for (std::size_t i = 0; i < N; ++i)
        if (same(text, names[i])) return static_cast<int>(i);
    return -1;
}

/// Moho's EAlliance, in its order (ALLIANCE_ prefix).
inline constexpr std::array<std::string_view, 3> kAllianceNames{"Neutral", "Ally", "Enemy"};

} // namespace osc::lua
