#define STB_TRUETYPE_IMPLEMENTATION
#include "stb/stb_truetype.h"

#include "ui/font_metrics_provider.hpp"
#include "vfs/virtual_file_system.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace osc::ui {

namespace {

/// GDI rounds a scaled design unit to the nearest pixel
i32 gdi_round(f32 v) {
    return static_cast<i32>(std::floor(v + 0.5f));
}

/// The tables GDI's text metrics come from. VDMX's group for square pixels
/// is the first whose ratio range holds 1:1, or one for every ratio (0:0).
GdiFontTables read_gdi_tables(const stbtt_fontinfo& info) {
    GdiFontTables t;
    stbtt_uint8* data = info.data;
    t.units_per_em = ttUSHORT(data + info.head + 18);
    t.hhea_ascender = ttSHORT(data + info.hhea + 4);
    t.hhea_descender = -ttSHORT(data + info.hhea + 6);
    t.hhea_line_gap = ttSHORT(data + info.hhea + 8);
    t.win_ascent = t.hhea_ascender;
    t.win_descent = t.hhea_descender;
    if (const stbtt_uint32 os2 = stbtt__find_table(data, info.fontstart, "OS/2")) {
        t.win_ascent = ttUSHORT(data + os2 + 74);
        t.win_descent = ttUSHORT(data + os2 + 76);
    }
    const stbtt_uint32 vdmx = stbtt__find_table(data, info.fontstart, "VDMX");
    if (vdmx == 0) {
        return t;
    }
    const i32 ratios = ttUSHORT(data + vdmx + 4);
    for (i32 i = 0; i < ratios; ++i) {
        const stbtt_uint8* r = data + vdmx + 6 + 4 * i;
        const bool any = r[1] == 0 && r[2] == 0 && r[3] == 0;
        if (!any && (r[2] > r[1] || r[1] > r[3])) {
            continue;
        }
        stbtt_uint8* group = data + vdmx + ttUSHORT(data + vdmx + 6 + 4 * ratios + 2 * i);
        const i32 records = ttUSHORT(group);
        for (i32 k = 0; k < records; ++k) {
            stbtt_uint8* rec = group + 4 + 6 * k;
            t.vdmx[ttUSHORT(rec)] = {ttSHORT(rec + 2), -ttSHORT(rec + 4)};
        }
        break;
    }
    return t;
}

} // namespace

GdiTextMetrics gdi_text_metrics(const GdiFontTables& t, i32 ppem) {
    GdiTextMetrics m;
    if (t.units_per_em <= 0) {
        return m;
    }
    const f32 scale = static_cast<f32>(ppem) / static_cast<f32>(t.units_per_em);
    m.ascent = gdi_round(static_cast<f32>(t.win_ascent) * scale);
    m.descent = gdi_round(static_cast<f32>(t.win_descent) * scale);
    if (auto it = t.vdmx.find(ppem); it != t.vdmx.end()) {
        m.ascent = it->second.first;
        m.descent = it->second.second;
    }
    // The hhea line gap less what the win ascent and descent already add
    const i32 gap =
        t.hhea_line_gap - ((t.win_ascent + t.win_descent) - (t.hhea_ascender + t.hhea_descender));
    m.external_leading = std::max(0, gdi_round(static_cast<f32>(gap) * scale));
    return m;
}

i32 gdi_line_height(const GdiFontTables& t, i32 ppem) {
    const GdiTextMetrics m = gdi_text_metrics(t, ppem);
    return m.ascent + m.descent + m.external_leading;
}

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

    cf.gdi = read_gdi_tables(*info);
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

    CachedMetrics cm;
    cm.scale = scale;

    metrics_cache_[key] = cm;
    return &metrics_cache_[key];
}

bool FontMetricsProvider::get_metrics(const std::string& family, i32 pointsize,
                                       Metrics& out) {
    CachedFont* cf = load_ttf(resolve_font_path(family));
    if (!cf) return false;
    const GdiTextMetrics m = gdi_text_metrics(cf->gdi, pointsize);
    out = {static_cast<f32>(m.ascent), static_cast<f32>(m.descent),
           static_cast<f32>(m.external_leading)};
    return true;
}

f32 FontMetricsProvider::line_height(const std::string& family, i32 pointsize) {
    CachedFont* cf = load_ttf(resolve_font_path(family));
    if (!cf) {
        return -1.0f;
    }
    return static_cast<f32>(gdi_line_height(cf->gdi, pointsize));
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
