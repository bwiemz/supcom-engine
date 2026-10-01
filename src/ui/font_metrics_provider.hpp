#pragma once

#include "core/types.hpp"

#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace osc::vfs {
class VirtualFileSystem;
}

namespace osc::ui {

/// A font's vertical metrics as GDI reads them: head, hhea, OS/2 and VDMX
struct GdiFontTables {
    i32 units_per_em = 0;
    i32 hhea_ascender = 0;
    i32 hhea_descender = 0; // positive below the baseline
    i32 hhea_line_gap = 0;
    i32 win_ascent = 0;
    i32 win_descent = 0;
    /// VDMX's group for square pixels: the hinted ascent and descent per ppem
    std::map<i32, std::pair<i32, i32>> vdmx;
};

/// A line's height in GDI's text metrics, tmHeight + tmExternalLeading, at
/// `ppem` pixels per em: how Moho spaces an ItemList's rows
i32 gdi_line_height(const GdiFontTables& tables, i32 ppem);

/// CPU-only font metrics using stb_truetype. No GPU resources needed.
/// Shared by moho_bindings (Lua-side metrics) and renderer::FontCache (GPU text).
class FontMetricsProvider {
public:
    struct Metrics {
        f32 ascent;
        f32 descent;
        f32 external_leading;
    };

    /// Set the VFS to load font files from.
    void set_vfs(vfs::VirtualFileSystem* vfs) { vfs_ = vfs; }

    /// Get metrics for a font family at a given pointsize.
    /// Returns false if the font cannot be loaded (caller should use heuristics).
    bool get_metrics(const std::string& family, i32 pointsize, Metrics& out);

    /// Compute pixel width of a string at a given font and size.
    /// Returns negative if font unavailable (caller should use heuristic).
    f32 string_advance(const std::string& family, i32 pointsize,
                       const std::string& text);

    /// The height GDI gives a line of the font at `pointsize`
    /// (gdi_line_height); negative if the font is unavailable.
    f32 line_height(const std::string& family, i32 pointsize);

    /// Singleton access (one per process is fine).
    static FontMetricsProvider& instance();

private:
    struct CachedFont {
        std::vector<char> ttf_data;
        // stb_truetype fontinfo is stored as opaque bytes to avoid
        // exposing stb_truetype.h in the header
        std::vector<u8> fontinfo_storage;
        GdiFontTables gdi;
        bool valid = false;
    };

    struct CachedMetrics {
        f32 scale;
        Metrics metrics;
    };

    std::string resolve_font_path(const std::string& family) const;
    CachedFont* load_ttf(const std::string& font_path);
    CachedMetrics* get_or_compute(const std::string& family, i32 pointsize);

    std::unordered_map<std::string, CachedFont> font_cache_;
    // key: "family:pointsize"
    std::unordered_map<std::string, CachedMetrics> metrics_cache_;
    vfs::VirtualFileSystem* vfs_ = nullptr;
};

} // namespace osc::ui
