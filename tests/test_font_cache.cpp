#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "renderer/font_cache.hpp"
#include "ui/font_metrics_provider.hpp"
#include "vfs/directory_mount.hpp"
#include "vfs/virtual_file_system.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <utility>

namespace fs = std::filesystem;

namespace {

/// Any TrueType font on this machine; the tests skip without one.
fs::path find_system_ttf() {
    for (const char* dir : {"/usr/share/fonts", "/usr/local/share/fonts"}) {
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(dir, ec);
             !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            auto ext = it->path().extension().string();
            if (ext == ".ttf" || ext == ".TTF") return it->path();
        }
    }
    return {};
}

} // namespace

TEST_CASE("FontCache: a space advances by the font's own width", "[font]") {
    const fs::path ttf = find_system_ttf();
    if (ttf.empty()) SKIP("no TrueType font installed");

    // Serve it as FA's Arial: /fonts/arial.ttf
    const fs::path root = fs::temp_directory_path() / "osc_font_cache_test";
    fs::remove_all(root);
    fs::create_directories(root / "fonts");
    fs::copy_file(ttf, root / "fonts" / "arial.ttf");
    osc::vfs::VirtualFileSystem vfs;
    vfs.mount("/", std::make_unique<osc::vfs::DirectoryMount>(root));

    osc::renderer::FontCache cache;
    cache.init(VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE,
               VK_NULL_HANDLE, VK_NULL_HANDLE, &vfs);
    const auto* atlas = cache.get("Arial", 12);
    REQUIRE(atlas);

    // The space has no pixels, but it is a glyph with an advance. Without
    // one the renderer spaced words by a 0.6-em guess, and drawn text ran
    // wider than the width the UI scripts laid it out at.
    auto space = atlas->glyphs.find(' ');
    REQUIRE(space != atlas->glyphs.end());
    CHECK(space->second.width == 0.0f);
    CHECK(space->second.x_advance > 0.0f);
    CHECK(space->second.x_advance < 12.0f * 0.6f);

    // Drawn width agrees with the width Lua measures (GetStringAdvance).
    osc::ui::FontMetricsProvider metrics;
    metrics.set_vfs(&vfs);
    const std::string text = "Armored Command Unit";
    CHECK_THAT(cache.string_advance("Arial", 12, text),
               Catch::Matchers::WithinAbs(metrics.string_advance("Arial", 12, text), 0.01));
    fs::remove_all(root);
}

TEST_CASE("A line is as tall as GDI's text metrics make it, as Moho spaces ItemList rows",
          "[font]") {
    // FA's Arial and Zeroes Three (UIUtil.titleFont): their head, hhea, OS/2
    // and VDMX values. Each row's height is retail's ItemList:GetRowHeight().
    osc::ui::GdiFontTables arial{2048, 1854, 434, 67, 1854, 434, {}};
    arial.vdmx = {{10, {10, 3}}, {12, {12, 3}}, {14, {13, 3}}, {16, {15, 3}},
                  {18, {17, 4}}, {20, {19, 4}}, {24, {21, 6}}};
    osc::ui::GdiFontTables zeroes{1000, 756, 195, 39, 910, 385, {}};
    zeroes.vdmx = {{10, {10, 4}}, {12, {11, 5}}, {14, {13, 6}}, {16, {15, 7}},
                   {18, {17, 7}}, {20, {19, 8}}, {24, {22, 10}}};

    // Arial's line gap adds a pixel from 16 points up
    const std::pair<int, int> arial_rows[] = {{10, 13}, {12, 15}, {14, 16}, {16, 19},
                                              {18, 22}, {20, 24}, {24, 28}};
    for (auto [size, row] : arial_rows) {
        CAPTURE(size);
        CHECK(osc::ui::gdi_line_height(arial, size) == row);
    }
    // Zeroes Three's win ascent and descent already hold its line gap
    const std::pair<int, int> zeroes_rows[] = {{10, 14}, {12, 16}, {14, 19}, {16, 22},
                                               {18, 24}, {20, 27}, {24, 32}};
    for (auto [size, row] : zeroes_rows) {
        CAPTURE(size);
        CHECK(osc::ui::gdi_line_height(zeroes, size) == row);
    }

    // A size VDMX doesn't list rounds the win ascent and descent
    CHECK(osc::ui::gdi_line_height(arial, 13) == 12 + 3 + 0);
}
