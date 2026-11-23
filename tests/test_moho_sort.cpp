#include <catch2/catch_test_macros.hpp>

#include "core/moho_sort.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

TEST_CASE("Moho's sort orders by the comparison and keeps every element", "[core]") {
    std::uint32_t state = 7;
    const auto next = [&state] {
        state = state * 1664525u + 1013904223u;
        return state >> 8;
    };
    for (int round = 0; round < 500; ++round) {
        const int n = static_cast<int>(next() % 120);
        std::vector<std::pair<int, int>> items;
        items.reserve(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) {
            items.emplace_back(static_cast<int>(next() % 11), i);
        }
        auto sorted = items;
        osc::moho_sort::sort(sorted.data(), sorted.data() + sorted.size(),
                             [](const auto& a, const auto& b) { return a.first < b.first; });
        INFO("round " << round << ", " << n << " items");
        CHECK(std::is_sorted(sorted.begin(), sorted.end(),
                             [](const auto& a, const auto& b) { return a.first < b.first; }));
        auto expect = items;
        std::sort(expect.begin(), expect.end());
        auto got = sorted;
        std::sort(got.begin(), got.end());
        CHECK(got == expect);
    }
}
