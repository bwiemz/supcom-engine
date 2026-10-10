#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/utf8.hpp"
#include "renderer/font_cache.hpp"
#include "ui/font_metrics_provider.hpp"
#include "vfs/directory_mount.hpp"
#include "vfs/virtual_file_system.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

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

/// A font with accented Latin and Cyrillic letters on this machine; the tests skip without one.
fs::path find_wide_ttf() {
    for (const char* dir : {"/usr/share/fonts", "/usr/local/share/fonts", "/Library/Fonts",
                            "/System/Library/Fonts/Supplemental", "C:/Windows/Fonts"}) {
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(dir, ec);
             !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            const auto name = it->path().filename().string();
            if (name == "DejaVuSans.ttf" || name == "LiberationSans-Regular.ttf" ||
                name == "Arial.ttf" || name == "arial.ttf") {
                return it->path();
            }
        }
    }
    return {};
}

/// FA's Arial: its head, hhea, OS/2 and VDMX values
osc::ui::GdiFontTables arial_tables() {
    osc::ui::GdiFontTables t{2048, 1854, 434, 67, 1854, 434, {}};
    t.vdmx = {{10, {10, 3}}, {12, {12, 3}}, {14, {13, 3}}, {16, {15, 3}},
              {18, {17, 4}}, {20, {19, 4}}, {24, {21, 6}}};
    return t;
}

/// FA's Zeroes Three (UIUtil.titleFont): its head, hhea, OS/2 and VDMX values
osc::ui::GdiFontTables zeroes_tables() {
    osc::ui::GdiFontTables t{1000, 756, 195, 39, 910, 385, {}};
    t.vdmx = {{10, {10, 4}}, {12, {11, 5}}, {14, {13, 6}}, {16, {15, 7}},
              {18, {17, 7}}, {20, {19, 8}}, {24, {22, 10}}};
    return t;
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
    // Each row's height is retail's ItemList:GetRowHeight().
    const osc::ui::GdiFontTables arial = arial_tables();
    const osc::ui::GdiFontTables zeroes = zeroes_tables();

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

TEST_CASE("A font's text metrics are GDI's whole pixels, as Moho's Text and Edit read them",
          "[font]") {
    // Retail's Text FontAscent, FontDescent and FontExternalLeading at each
    // size; its Height and Edit's GetFontHeight are the ascent and descent
    struct Expected {
        int size, ascent, descent, leading;
    };
    const Expected arial[] = {
        {10, 10, 3, 0}, {12, 12, 3, 0}, {14, 13, 3, 0}, {16, 15, 3, 1}, {18, 17, 4, 1}};
    const Expected zeroes[] = {
        {10, 10, 4, 0}, {12, 11, 5, 0}, {14, 13, 6, 0}, {16, 15, 7, 0}, {18, 17, 7, 0}};
    const auto check = [](const osc::ui::GdiFontTables& tables, const Expected& e) {
        CAPTURE(e.size);
        const auto m = osc::ui::gdi_text_metrics(tables, e.size);
        CHECK(m.ascent == e.ascent);
        CHECK(m.descent == e.descent);
        CHECK(m.external_leading == e.leading);
    };
    for (const auto& e : arial) {
        check(arial_tables(), e);
    }
    for (const auto& e : zeroes) {
        check(zeroes_tables(), e);
    }
}

TEST_CASE("UTF-8 text is read a character at a time and a stray byte stands for itself", "[font]") {
    const std::string text = "a\xD0\x96\xE2\x80\x94\xF0\x9F\x98\x80\xE9z\xD0";
    std::vector<osc::u32> read;
    for (size_t i = 0; i < text.size();) {
        read.push_back(osc::next_codepoint(text, i));
    }
    CHECK(read == std::vector<osc::u32>{'a', 0x416, 0x2014, 0x1F600, 0xE9, 'z', 0xD0});
}

TEST_CASE("A font's atlas holds its accented Latin and Cyrillic letters, and text is measured "
          "by character",
          "[font]") {
    const fs::path ttf = find_wide_ttf();
    if (ttf.empty()) {
        SKIP("no font with accented Latin and Cyrillic letters installed");
    }

    const fs::path root = fs::temp_directory_path() / "osc_font_cache_wide_test";
    fs::remove_all(root);
    fs::create_directories(root / "fonts");
    fs::copy_file(ttf, root / "fonts" / "arial.ttf");
    osc::vfs::VirtualFileSystem vfs;
    vfs.mount("/", std::make_unique<osc::vfs::DirectoryMount>(root));

    osc::renderer::FontCache cache;
    cache.init(VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE,
               VK_NULL_HANDLE, &vfs);
    const auto* atlas = cache.get("Arial", 20);
    REQUIRE(atlas);
    for (osc::u32 cp : {0xDFu, 0x141u, 0x10Cu}) {
        CHECK(atlas->glyphs.count(cp) == 1);
    }
    const auto zhe = atlas->glyphs.find(0x416);
    REQUIRE(zhe != atlas->glyphs.end());
    CHECK(zhe->second.width > 0.0f);
    CHECK(zhe->second.x_advance > 0.0f);

    osc::ui::FontMetricsProvider metrics;
    metrics.set_vfs(&vfs);
    const std::string text = "\xD0\x96\xD0\x96";
    CHECK_THAT(cache.string_advance("Arial", 20, text),
               Catch::Matchers::WithinAbs(2.0f * zhe->second.x_advance, 0.01));
    CHECK_THAT(metrics.string_advance("Arial", 20, text),
               Catch::Matchers::WithinAbs(2.0f * zhe->second.x_advance, 0.01));
    fs::remove_all(root);
}

TEST_CASE("A face the game's files lack is drawn as Arial", "[font]") {
    // FAF's UI asks for Calibri, which Moho takes from Windows' fonts.
    const fs::path ttf = find_system_ttf();
    if (ttf.empty()) SKIP("no TrueType font installed");
    const fs::path root = fs::temp_directory_path() / "osc_font_fallback_test";
    fs::remove_all(root);
    fs::create_directories(root / "fonts");
    fs::copy_file(ttf, root / "fonts" / "arial.ttf");
    osc::vfs::VirtualFileSystem vfs;
    vfs.mount("/", std::make_unique<osc::vfs::DirectoryMount>(root));

    osc::renderer::FontCache cache;
    cache.init(VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE,
               VK_NULL_HANDLE, &vfs);
    const auto* calibri = cache.get("Calibri", 14);
    REQUIRE(calibri);
    const auto* arial = cache.get("Arial", 14);
    REQUIRE(arial);
    const auto a_calibri = calibri->glyphs.find('A');
    const auto a_arial = arial->glyphs.find('A');
    REQUIRE(a_calibri != calibri->glyphs.end());
    REQUIRE(a_arial != arial->glyphs.end());
    CHECK(a_calibri->second.x_advance == a_arial->second.x_advance);
    // Asked again (another size too), it is found, not looked for afresh.
    CHECK(cache.get("Calibri", 14) == calibri);
    CHECK(cache.get("Calibri", 20) != nullptr);
    fs::remove_all(root);
}

TEST_CASE("Text laid out for the HUD stands on its font's baseline", "[font]") {
    const fs::path ttf = find_wide_ttf();
    if (ttf.empty()) {
        SKIP("no Arial-like font installed");
    }
    const fs::path root = fs::temp_directory_path() / "osc_font_baseline_test";
    fs::remove_all(root);
    fs::create_directories(root / "fonts");
    fs::copy_file(ttf, root / "fonts" / "arial.ttf");
    osc::vfs::VirtualFileSystem vfs;
    vfs.mount("/", std::make_unique<osc::vfs::DirectoryMount>(root));

    osc::renderer::FontCache cache;
    cache.init(VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE,
               VK_NULL_HANDLE, &vfs);
    const auto* atlas = cache.get("Arial", 13);
    REQUIRE(atlas);
    const float top = 40.0f;
    std::vector<std::pair<float, float>> spans;
    const float advance = osc::renderer::place_glyphs(
        *atlas, "HI", 10.0f, top, [&](float, float y, const osc::renderer::GlyphInfo& gi) {
            spans.emplace_back(y, y + gi.height);
        });
    REQUIRE(spans.size() == 2);
    for (const auto& [glyph_top, glyph_bottom] : spans) {
        CHECK(glyph_top >= top);
        CHECK(glyph_bottom == top + atlas->metrics.ascent);
    }
    CHECK(advance == cache.string_advance("Arial", 13, "HI"));
    fs::remove_all(root);
}
