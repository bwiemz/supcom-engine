#define STB_TRUETYPE_IMPLEMENTATION
#include "stb/stb_truetype.h"

#include "ui/font_metrics_provider.hpp"
#include "vfs/virtual_file_system.hpp"

#include <algorithm>
#include <cstring>

namespace osc::ui {

static_assert(sizeof(stbtt_fontinfo) <= 512,
              "stbtt_fontinfo size assumption — bump storage if needed");

FontMetricsProvider& FontMetricsProvider::instance() {
    static FontMetricsProvider s_instance;
    return s_instance;
}

std::string FontMetricsProvider::resolve_font_path(const std::string& family) const {
    std::string lower = family;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    if (lower == "arial" || lower.empty())
        return "/fonts/ARIAL.TTF";
    if (lower == "arial bold" || lower == "arial bd")
        return "/fonts/ARIALBD.TTF";
    if (lower == "arial italic")
        return "/fonts/ARIALI.TTF";
    if (lower == "arial narrow")
        return "/fonts/ARIALN.TTF";
    if (lower == "arial black")
        return "/fonts/ARIBLK.TTF";
    if (lower == "butterbe")
        return "/fonts/BUTTERBE.TTF";
    if (lower == "zeroes three" || lower == "zeroes_3")
        return "/fonts/zeroes_3.ttf";
    if (lower == "wintermu" || lower == "wintermute")
        return "/fonts/wintermu.ttf";
    if (lower == "arlrdbd")
        return "/fonts/ARLRDBD.TTF";
    if (lower == "vdub")
        return "/fonts/vdub.ttf";

    return "/fonts/" + family + ".TTF";
}

FontMetricsProvider::CachedFont* FontMetricsProvider::load_ttf(const std::string& font_path) {
    auto it = font_cache_.find(font_path);
    if (it != font_cache_.end()) {
        return it->second.valid ? &it->second : nullptr;
    }

    CachedFont cf;
    cf.valid = false;

    if (!vfs_) {
        font_cache_[font_path] = std::move(cf);
        return nullptr;
    }

    auto data = vfs_->read_file(font_path);
    if (!data || data->empty()) {
        // Try lowercase
        std::string lower_path = font_path;
        std::transform(lower_path.begin(), lower_path.end(),
                       lower_path.begin(), ::tolower);
        data = vfs_->read_file(lower_path);
        if (!data || data->empty()) {
            font_cache_[font_path] = std::move(cf);
            return nullptr;
        }
    }

    cf.ttf_data = std::move(*data);
    cf.fontinfo_storage.resize(sizeof(stbtt_fontinfo), 0);

    auto* info = reinterpret_cast<stbtt_fontinfo*>(cf.fontinfo_storage.data());
    if (!stbtt_InitFont(info,
                        reinterpret_cast<const unsigned char*>(cf.ttf_data.data()),
                        stbtt_GetFontOffsetForIndex(
                            reinterpret_cast<const unsigned char*>(cf.ttf_data.data()), 0))) {
        font_cache_[font_path] = std::move(cf);
        return nullptr;
    }

    cf.valid = true;
    font_cache_[font_path] = std::move(cf);
    return &font_cache_[font_path];
}

FontMetricsProvider::CachedMetrics* FontMetricsProvider::get_or_compute(
        const std::string& family, i32 pointsize) {
    std::string key = family + ":" + std::to_string(pointsize);
    auto it = metrics_cache_.find(key);
    if (it != metrics_cache_.end()) return &it->second;

    std::string font_path = resolve_font_path(family);
    CachedFont* cf = load_ttf(font_path);
    if (!cf) return nullptr;

    auto* info = reinterpret_cast<stbtt_fontinfo*>(cf->fontinfo_storage.data());
    f32 scale = stbtt_ScaleForPixelHeight(info, static_cast<f32>(pointsize));

    int ascent_raw, descent_raw, line_gap_raw;
    stbtt_GetFontVMetrics(info, &ascent_raw, &descent_raw, &line_gap_raw);

    CachedMetrics cm;
    cm.scale = scale;
    cm.metrics.ascent = ascent_raw * scale;
    cm.metrics.descent = -descent_raw * scale; // stb gives negative descent
    cm.metrics.external_leading = line_gap_raw * scale;

    metrics_cache_[key] = cm;
    return &metrics_cache_[key];
}

bool FontMetricsProvider::get_metrics(const std::string& family, i32 pointsize,
                                       Metrics& out) {
    auto* cm = get_or_compute(family, pointsize);
    if (!cm) return false;
    out = cm->metrics;
    return true;
}

f32 FontMetricsProvider::string_advance(const std::string& family, i32 pointsize,
                                         const std::string& text) {
    std::string font_path = resolve_font_path(family);
    CachedFont* cf = load_ttf(font_path);
    if (!cf) return -1.0f;

    auto* info = reinterpret_cast<stbtt_fontinfo*>(cf->fontinfo_storage.data());

    // Get or compute scale
    auto* cm = get_or_compute(family, pointsize);
    if (!cm) return -1.0f;
    f32 scale = cm->scale;

    f32 advance = 0.0f;
    for (unsigned char c : text) {
        int adv_raw, lsb;
        stbtt_GetCodepointHMetrics(info, static_cast<int>(c), &adv_raw, &lsb);
        advance += adv_raw * scale;
    }
    return advance;
}

} // namespace osc::ui
