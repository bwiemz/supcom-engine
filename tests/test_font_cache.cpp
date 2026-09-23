#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "renderer/font_cache.hpp"
#include "ui/font_metrics_provider.hpp"
#include "vfs/directory_mount.hpp"
#include "vfs/virtual_file_system.hpp"

#include <filesystem>
#include <memory>
#include <string>

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
