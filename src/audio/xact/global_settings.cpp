#include "audio/xact/global_settings.hpp"

#include "audio/xact/byte_reader.hpp"

#include <algorithm>
#include <cmath>

namespace osc::audio::xact {

f32 millibels_to_gain(f32 mb) {
    if (mb <= kSilenceMb) return 0.0f;
    return std::pow(10.0f, mb / 2000.0f);
}

f32 volume_byte_to_millibels(u8 b) {
    // The XACT builder's own mapping (fitted by FAudio from the tool's
    // byte table): 180 is 0 mB, 255 about +6 dB, 0 silence.
    if (b == 0) return kSilenceMb;
    return static_cast<f32>(3969.0 * std::log10(b / 28240.0) + 8715.0);
}

f32 RpcCurve::evaluate(f32 x) const {
    if (points.empty()) return 0.0f;
    if (x <= points.front().x) return points.front().y;
    if (x >= points.back().x) return points.back().y;
    for (size_t i = 0; i + 1 < points.size(); ++i) {
        const Point& a = points[i];
        const Point& b = points[i + 1];
        if (x < a.x || x > b.x) continue;
        const f32 span = b.x - a.x;
        const f32 t = span > 0 ? (x - a.x) / span : 1.0f;
        const f32 dy = b.y - a.y;
        switch (a.type) {
        case PointType::Fast:
            return a.y + dy * (1.0f - std::pow(1.0f - std::pow(t, 1.0f / 1.5f), 1.5f));
        case PointType::Slow:
            return a.y + dy * (1.0f - std::pow(1.0f - std::pow(t, 1.5f), 1.0f / 1.5f));
        case PointType::SinCos:
            return dy > 0 ? a.y + dy * (1.0f - std::pow(1.0f - std::sqrt(t), 2.0f))
                          : a.y + dy * (1.0f - std::sqrt(1.0f - t * t));
        case PointType::Linear:
        default:
            return a.y + dy * t;
        }
    }
    return points.back().y;
}

int GlobalSettings::find_category(std::string_view name) const {
    for (size_t i = 0; i < categories.size(); ++i)
        if (categories[i].name == name) return static_cast<int>(i);
    return -1;
}

int GlobalSettings::find_variable(std::string_view name) const {
    for (size_t i = 0; i < variables.size(); ++i)
        if (variables[i].name == name) return static_cast<int>(i);
    return -1;
}

const RpcCurve* GlobalSettings::rpc(u32 code) const {
    for (const auto& r : rpcs)
        if (r.code == code) return &r;
    return nullptr;
}

Result<GlobalSettings> GlobalSettings::parse(std::span<const u8> data) {
    ByteReader r(data);
    if (r.read_u32() != 0x46534758) return Error("XGS: not an XACT global settings file");
    const u16 content = r.read_u16();
    if (content < 43 || content > 46) return Error("XGS: unsupported content version");
    r.skip(2 + 2 + 8); // tool version, unknown, last modified
    r.skip(1);         // platform / XACT version

    const u16 category_count = r.read_u16();
    const u16 variable_count = r.read_u16();
    r.skip(2 + 2); // blob counts
    const u16 rpc_count = r.read_u16();
    r.skip(2 + 2); // DSP presets, parameters (FA has none)

    const u32 category_off = r.read_u32();
    const u32 variable_off = r.read_u32();
    r.skip(4); // blob 1
    const u32 category_name_index_off = r.read_u32();
    r.skip(4); // blob 2
    const u32 variable_name_index_off = r.read_u32();
    r.skip(4 + 4); // name tables (reached through the indices)
    const u32 rpc_off = r.read_u32();
    if (!r.ok()) return Error("XGS: truncated header");

    GlobalSettings gs;
    auto name_at = [&](u32 index_off, u16 i) {
        ByteReader idx(data, index_off + static_cast<size_t>(i) * 6);
        const u32 at = idx.read_u32();
        if (!idx.ok()) r.seek(data.size() + 1); // poison
        return r.cstr(at);
    };

    r.seek(category_off);
    gs.categories.resize(category_count);
    for (u16 i = 0; i < category_count; ++i) {
        Category& c = gs.categories[i];
        c.instance_limit = r.read_u8();
        c.fade_in_ms = r.read_u16();
        c.fade_out_ms = r.read_u16();
        c.limit_behavior = static_cast<LimitBehavior>(r.read_u8() >> 3);
        c.parent = r.read_u16();
        c.volume_mb = volume_byte_to_millibels(r.read_u8());
        r.skip(1); // visibility
    }
    r.seek(variable_off);
    gs.variables.resize(variable_count);
    for (u16 i = 0; i < variable_count; ++i) {
        Variable& v = gs.variables[i];
        v.accessibility = r.read_u8();
        v.initial = r.read_f32();
        v.min = r.read_f32();
        v.max = r.read_f32();
    }
    if (rpc_count > 0) {
        r.seek(rpc_off);
        gs.rpcs.resize(rpc_count);
        for (u16 i = 0; i < rpc_count; ++i) {
            RpcCurve& c = gs.rpcs[i];
            c.code = static_cast<u32>(r.pos());
            c.variable = r.read_u16();
            const u8 point_count = r.read_u8();
            c.parameter = static_cast<RpcCurve::Parameter>(r.read_u16());
            c.points.resize(point_count);
            for (auto& p : c.points) {
                p.x = r.read_f32();
                p.y = r.read_f32();
                p.type = static_cast<RpcCurve::PointType>(r.read_u8());
            }
        }
    }
    for (u16 i = 0; i < category_count; ++i)
        gs.categories[i].name = name_at(category_name_index_off, i);
    for (u16 i = 0; i < variable_count; ++i)
        gs.variables[i].name = name_at(variable_name_index_off, i);
    if (!r.ok()) return Error("XGS: truncated or malformed data");

    for (const auto& c : gs.categories) {
        if (c.parent != 0xFFFF && c.parent >= category_count)
            return Error("XGS: category '" + c.name + "' has a bad parent");
    }
    for (const auto& c : gs.rpcs) {
        if (c.variable >= variable_count) return Error("XGS: RPC on a missing variable");
    }
    return gs;
}

} // namespace osc::audio::xact
