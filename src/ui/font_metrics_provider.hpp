#pragma once

#include "core/types.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace osc::vfs {
class VirtualFileSystem;
}

namespace osc::ui {

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

    /// Singleton access (one per process is fine).
    static FontMetricsProvider& instance();

private:
    struct CachedFont {
        std::vector<char> ttf_data;
        // stb_truetype fontinfo is stored as opaque bytes to avoid
        // exposing stb_truetype.h in the header
        std::vector<u8> fontinfo_storage;
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
