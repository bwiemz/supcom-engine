#include "map/scmap_parser.hpp"

#include <spdlog/spdlog.h>

#include <cstring>
#include <string>


namespace osc::map {

namespace {

/// Simple binary reader that tracks offset into a byte buffer.
class BinaryReader {
public:
    BinaryReader(const u8* data, size_t size)
        : data_(data), size_(size), pos_(0) {}

    bool has_remaining(size_t bytes) const { return pos_ + bytes <= size_; }
    size_t position() const { return pos_; }
    size_t remaining() const { return size_ - pos_; }

    i32 read_i32() {
        i32 val;
        std::memcpy(&val, data_ + pos_, 4);
        pos_ += 4;
        return val;
    }

    u32 read_u32() {
        u32 val;
        std::memcpy(&val, data_ + pos_, 4);
        pos_ += 4;
        return val;
    }

    i16 read_i16() {
        i16 val;
        std::memcpy(&val, data_ + pos_, 2);
        pos_ += 2;
        return val;
    }

    u16 read_u16() {
        u16 val;
        std::memcpy(&val, data_ + pos_, 2);
        pos_ += 2;
        return val;
    }

    f32 read_f32() {
        f32 val;
        std::memcpy(&val, data_ + pos_, 4);
        pos_ += 4;
        return val;
    }

    u8 read_u8() {
        return data_[pos_++];
    }

    void skip(size_t bytes) {
        pos_ += bytes;
    }

    /// Read null-terminated C string, advancing past the null byte.
    std::string read_cstring() {
        std::string result;
        while (pos_ < size_ && data_[pos_] != 0) {
            result.push_back(static_cast<char>(data_[pos_++]));
        }
        if (pos_ < size_) pos_++; // skip null terminator
        return result;
    }

    /// Read raw bytes as vector<char> (matches parse_dds API).
    /// Clamps to remaining data to prevent buffer overread.
    std::vector<char> read_bytes(size_t count) {
        size_t avail = (pos_ < size_) ? (size_ - pos_) : 0;
        count = std::min(count, avail);
        std::vector<char> result(count);
        if (count > 0)
            std::memcpy(result.data(), data_ + pos_, count);
        pos_ += count;
        return result;
    }

    /// Read Int16 array of count elements.
    std::vector<i16> read_i16_array(size_t count) {
        std::vector<i16> result(count);
        std::memcpy(result.data(), data_ + pos_, count * sizeof(i16));
        pos_ += count * sizeof(i16);
        return result;
    }

    /// Read UInt16 array of count elements.
    std::vector<u16> read_u16_array(size_t count) {
        std::vector<u16> result(count);
        std::memcpy(result.data(), data_ + pos_, count * sizeof(u16));
        pos_ += count * sizeof(u16);
        return result;
    }

    const u8* data_at(size_t offset) const { return data_ + offset; }

private:
    const u8* data_;
    size_t size_;
    size_t pos_;
};

/// Skip from after water elevation data to the props section,
/// capturing stratum metadata and blend DDS along the way.
/// Returns true if successful, false if data is truncated/malformed.
bool skip_to_props(BinaryReader& r, i32 version_minor, u32 map_width, u32 map_height,
                   bool has_water, ScmapData& result) {
    // --- Water properties (only present when has_water is true) ---
    if (has_water) {
        // 20 floats: surfaceColor(3), colorLerpMin, colorLerpMax, refraction,
        // fresnelBias, fresnelPower, reflectionUnit, reflectionSky,
        // sunShininess, sunStrength, sunGlow, unknown8, unknown9,
        // sunColor(3), reflectionSun, unknown10
        if (!r.has_remaining(80)) return false;
        r.skip(80);

        // 2 water texture paths (cubemap, ramp)
        r.read_cstring();
        r.read_cstring();

        // 4 wave normal frequencies
        if (!r.has_remaining(16)) return false;
        r.skip(16);

        // 4 wave textures: each has scaleX(f32), scaleY(f32), path(cstring)
        for (int i = 0; i < 4; i++) {
            if (!r.has_remaining(8)) return false;
            r.skip(8); // scaleX + scaleY
            r.read_cstring(); // wave texture path
        }

        // --- Wave generators ---
        if (!r.has_remaining(4)) return false;
        u32 wave_gen_count = r.read_u32();
        if (wave_gen_count > 10000) return false; // sanity
        for (u32 i = 0; i < wave_gen_count; i++) {
            r.read_cstring(); // texture name
            r.read_cstring(); // ramp name
            // position(3f) + rotation(f) + velocity(3f) + 10 floats
            if (!r.has_remaining(68)) return false;
            r.skip(68);
        }
    }

    // --- Version-dependent tileset data ---
    if (version_minor >= 59) {
        if (!r.has_remaining(28)) return false;
        r.skip(28);
    } else if (version_minor > 53) {
        if (!r.has_remaining(24)) return false;
        r.skip(24);
    } else {
        r.read_cstring(); // tileset name
    }

    // --- Terrain strata ---
    if (version_minor > 53) {
        result.strata.resize(10);
        // 10 strata albedo: each cstring + f32
        for (int i = 0; i < 10; i++) {
            result.strata[i].albedo_path = r.read_cstring();
            if (!r.has_remaining(4)) return false;
            result.strata[i].albedo_scale = r.read_f32();
        }
        // 9 strata normals (UpperStratum excluded): each cstring + f32
        for (int i = 0; i < 9; i++) {
            result.strata[i].normal_path = r.read_cstring();
            if (!r.has_remaining(4)) return false;
            result.strata[i].normal_scale = r.read_f32();
        }
    } else {
        if (!r.has_remaining(4)) return false;
        u32 strata_count = r.read_u32();
        if (strata_count > 100) return false;
        result.strata.resize(strata_count);
        for (u32 i = 0; i < strata_count; i++) {
            result.strata[i].albedo_path = r.read_cstring();
            result.strata[i].normal_path = r.read_cstring();
            if (!r.has_remaining(8)) return false;
            result.strata[i].albedo_scale = r.read_f32();
            result.strata[i].normal_scale = r.read_f32();
        }
    }

    // --- Two unknown u32s ---
    if (!r.has_remaining(8)) return false;
    r.skip(8);

    // --- Decals ---
    if (!r.has_remaining(4)) return false;
    u32 decal_count = r.read_u32();
    if (decal_count > 100000) return false;
    result.decals.reserve(decal_count);
    auto normalize_path = [](std::string& s) {
        for (char& c : s)
            if (c == '\\') c = '/';
    };
    for (u32 i = 0; i < decal_count; i++) {
        ScmapDecal d;
        // decalId(u32) + decalType(u32) + unknown(u32)
        if (!r.has_remaining(12)) return false;
        d.decal_id = r.read_u32();
        d.decal_type = r.read_u32();
        r.read_u32(); // unknown
        // texture1: length-prefixed
        if (!r.has_remaining(4)) return false;
        u32 tex1_len = r.read_u32();
        if (tex1_len > 10000) return false;
        if (!r.has_remaining(tex1_len)) return false;
        if (tex1_len > 0) {
            d.texture1_path.assign(
                reinterpret_cast<const char*>(r.data_at(r.position())),
                tex1_len);
            r.skip(tex1_len);
            normalize_path(d.texture1_path);
        }
        // texture2: length-prefixed
        if (!r.has_remaining(4)) return false;
        u32 tex2_len = r.read_u32();
        if (tex2_len > 10000) return false;
        if (tex2_len > 0) {
            if (!r.has_remaining(tex2_len)) return false;
            d.texture2_path.assign(
                reinterpret_cast<const char*>(r.data_at(r.position())),
                tex2_len);
            r.skip(tex2_len);
            normalize_path(d.texture2_path);
        }
        // scale(3f) + position(3f) + rotation(3f) + cutOffLOD(f) + nearCutOffLOD(f) + removeTick(u32)
        if (!r.has_remaining(48)) return false;
        d.scale_x = r.read_f32(); d.scale_y = r.read_f32(); d.scale_z = r.read_f32();
        d.position_x = r.read_f32(); d.position_y = r.read_f32(); d.position_z = r.read_f32();
        d.rotation_x = r.read_f32(); d.rotation_y = r.read_f32(); d.rotation_z = r.read_f32();
        d.cut_off_lod = r.read_f32();
        d.near_cut_off_lod = r.read_f32();
        d.remove_tick = r.read_u32();
        result.decals.push_back(std::move(d));
    }

    // --- Decal groups ---
    if (!r.has_remaining(4)) return false;
    u32 decal_group_count = r.read_u32();
    if (decal_group_count > 10000) return false;
    for (u32 i = 0; i < decal_group_count; i++) {
        if (!r.has_remaining(4)) return false;
        r.skip(4); // group id
        r.read_cstring(); // group name
        if (!r.has_remaining(4)) return false;
        u32 entry_count = r.read_u32();
        if (entry_count > 100000) return false;
        if (!r.has_remaining(entry_count * 4)) return false;
        r.skip(entry_count * 4); // entry u32s
    }

    // --- Normal maps (width, height, count + length-prefixed DDS blobs) ---
    if (!r.has_remaining(12)) return false;
    r.skip(8); // width + height
    u32 normal_map_count = r.read_u32();
    if (normal_map_count > 100) return false;
    for (u32 i = 0; i < normal_map_count; i++) {
        if (!r.has_remaining(4)) return false;
        u32 data_len = r.read_u32();
        if (!r.has_remaining(data_len)) return false;
        r.skip(data_len);
    }

    // --- Version <56: extra u32 ---
    if (version_minor < 56) {
        if (!r.has_remaining(4)) return false;
        r.skip(4);
    }

    // --- Stratum 1-4 DDS (length-prefixed) ---
    if (!r.has_remaining(4)) return false;
    {
        u32 len = r.read_u32();
        if (!r.has_remaining(len)) return false;
        result.blend_dds_0 = r.read_bytes(len);
    }

    // --- Version <56: extra u32 ---
    if (version_minor < 56) {
        if (!r.has_remaining(4)) return false;
        r.skip(4);
    }

    // --- Stratum 5-8 DDS (length-prefixed) ---
    if (!r.has_remaining(4)) return false;
    {
        u32 len = r.read_u32();
        if (!r.has_remaining(len)) return false;
        result.blend_dds_1 = r.read_bytes(len);
    }

    // --- Version >53: water brush DDS (u32 unknown + length-prefixed DDS) ---
    if (version_minor > 53) {
        if (!r.has_remaining(8)) return false;
        r.skip(4); // unknown u32
        u32 len = r.read_u32();
        if (!r.has_remaining(len)) return false;
        r.skip(len);
    }

    // --- Water maps: 3 × (map_width/2 × map_height/2) bytes ---
    size_t water_map_size = static_cast<size_t>(map_width / 2) *
                            static_cast<size_t>(map_height / 2);
    size_t total_water_maps = water_map_size * 3; // foam + flatness + depth bias
    if (!r.has_remaining(total_water_maps)) return false;
    r.skip(total_water_maps);

    // --- Terrain type data: map_width × map_height bytes ---
    size_t terrain_type_size = static_cast<size_t>(map_width) *
                               static_cast<size_t>(map_height);
    if (!r.has_remaining(terrain_type_size)) return false;
    r.skip(terrain_type_size);

    // --- Version <53: extra i16 ---
    if (version_minor < 53) {
        if (!r.has_remaining(2)) return false;
        r.skip(2);
    }

    // --- Version >=59: extended metadata ---
    if (version_minor >= 59) {
        if (!r.has_remaining(64)) return false;
        r.skip(64);
        r.read_cstring(); // unknown string 1
        r.read_cstring(); // unknown string 2
        if (!r.has_remaining(4)) return false;
        u32 extra_count = r.read_u32();
        if (extra_count > 10000) return false;
        size_t extra_bytes = static_cast<size_t>(extra_count) * 40;
        if (!r.has_remaining(extra_bytes)) return false;
        r.skip(extra_bytes);
        if (!r.has_remaining(19)) return false;
        r.skip(19);
        r.read_cstring(); // unknown string 3
        if (!r.has_remaining(88)) return false;
        r.skip(88);
    }

    return true;
}

} // namespace

Result<ScmapData> parse_scmap(const std::vector<u8>& file_data) {
    if (file_data.size() < 30) {
        return Error("SCMAP file too small");
    }

    BinaryReader r(file_data.data(), file_data.size());

    // --- Header ---
    // Magic: "Map\x1a"
    u8 magic[4];
    magic[0] = r.read_u8();
    magic[1] = r.read_u8();
    magic[2] = r.read_u8();
    magic[3] = r.read_u8();
    if (magic[0] != 'M' || magic[1] != 'a' || magic[2] != 'p' ||
        magic[3] != 0x1a) {
        return Error("Invalid SCMAP magic (expected 'Map\\x1a')");
    }

    i32 version_major = r.read_i32();
    (void)version_major;

    r.skip(4); // unknown
    r.skip(4); // unknown

    f32 scaled_width = r.read_f32();
    f32 scaled_height = r.read_f32();
    (void)scaled_width;
    (void)scaled_height;

    r.skip(4); // unknown
    r.skip(2); // unknown (int16)

    // --- Preview image ---
    if (!r.has_remaining(4)) return Error("SCMAP truncated before preview length");
    i32 preview_length = r.read_i32();
    if (preview_length < 0 || !r.has_remaining(static_cast<size_t>(preview_length))) {
        return Error("SCMAP invalid preview image length");
    }
    auto preview_dds = r.read_bytes(static_cast<size_t>(preview_length));

    // --- Version + dimensions ---
    if (!r.has_remaining(16)) return Error("SCMAP truncated before dimensions");
    i32 version_minor = r.read_i32();
    u32 map_width = static_cast<u32>(r.read_i32());
    u32 map_height = static_cast<u32>(r.read_i32());

    // Sanity check dimensions
    if (map_width == 0 || map_height == 0 || map_width > 4096 ||
        map_height > 4096) {
        return Error("SCMAP invalid dimensions: " + std::to_string(map_width) +
                     "x" + std::to_string(map_height));
    }

    // --- Heightmap ---
    f32 height_scale = r.read_f32();
    size_t grid_w = static_cast<size_t>(map_width) + 1;
    size_t grid_h = static_cast<size_t>(map_height) + 1;
    size_t heightmap_count = grid_w * grid_h;
    size_t heightmap_bytes = heightmap_count * 2;

    if (!r.has_remaining(heightmap_bytes)) {
        return Error("SCMAP truncated in heightmap data (need " +
                     std::to_string(heightmap_bytes) + " bytes, have " +
                     std::to_string(r.remaining()) + ")");
    }
    std::vector<u16> heightmap = r.read_u16_array(heightmap_count);

    // --- Skip shader/environment strings to reach water data ---
    // Format: null_byte + shader(cstr) + background(cstr) + sky(cstr)
    //         + int32 env_count + (name_cstr, file_cstr) * env_count
    if (!r.has_remaining(1)) return Error("SCMAP truncated before shader section");
    r.skip(1); // unknown flag byte before shader strings

    r.read_cstring(); // terrain shader
    r.read_cstring(); // background texture
    r.read_cstring(); // sky cubemap

    // Environment cubemaps: int32 count + (name, file) pairs per entry
    if (!r.has_remaining(4)) return Error("SCMAP truncated before env cubemap count");
    i32 env_cubemap_count = r.read_i32();
    if (env_cubemap_count < 0 || env_cubemap_count > 128) {
        return Error("SCMAP invalid env cubemap count: " + std::to_string(env_cubemap_count));
    }
    for (i32 i = 0; i < env_cubemap_count; i++) {
        r.read_cstring(); // cubemap name
        r.read_cstring(); // cubemap file
    }

    // Lighting data: 23 floats = 92 bytes
    if (!r.has_remaining(92)) return Error("SCMAP truncated before lighting data");
    r.skip(92);

    // --- Water ---
    bool has_water = false;
    f32 water_elevation = 0.0f;
    f32 water_deep_elevation = 0.0f;
    f32 water_abyss_elevation = 0.0f;

    if (r.has_remaining(1)) {
        has_water = r.read_u8() != 0;
    }
    if (has_water && r.has_remaining(12)) {
        water_elevation = r.read_f32();
        water_deep_elevation = r.read_f32();
        water_abyss_elevation = r.read_f32();
    }

    // --- Build result with heightmap + water (always available) ---
    ScmapData result;
    result.map_width = map_width;
    result.map_height = map_height;
    result.height_scale = height_scale;
    result.heightmap = std::move(heightmap);
    result.has_water = has_water;
    result.water_elevation = water_elevation;
    result.water_deep_elevation = water_deep_elevation;
    result.water_abyss_elevation = water_abyss_elevation;
    result.version_minor = version_minor;
    result.preview_dds = std::move(preview_dds);

    // --- Skip intermediate sections to reach props ---
    // Graceful degradation: if skip fails, return result without props
    if (!skip_to_props(r, version_minor, map_width, map_height, has_water, result)) {
        spdlog::warn("SCMAP: failed to skip to props section at offset {} "
                     "(remaining {} bytes) — props not loaded",
                     r.position(), r.remaining());
        return result;
    }

    // --- Parse props ---
    if (!r.has_remaining(4)) {
        spdlog::warn("SCMAP: truncated before prop count");
        return result;
    }
    u32 prop_count = r.read_u32();
    if (prop_count > 100000) {
        spdlog::warn("SCMAP: unreasonable prop count {} — skipping", prop_count);
        return result;
    }

    result.props.reserve(prop_count);
    for (u32 i = 0; i < prop_count; i++) {
        if (!r.has_remaining(1)) { // at minimum need a cstring
            spdlog::warn("SCMAP: truncated at prop {}/{}", i, prop_count);
            break;
        }
        ScmapProp p;
        p.blueprint_path = r.read_cstring();
        if (!r.has_remaining(60)) { // 15 floats: pos(3) + rot(9) + scale(3)
            spdlog::warn("SCMAP: truncated at prop {}/{} data", i, prop_count);
            break;
        }
        p.px = r.read_f32(); p.py = r.read_f32(); p.pz = r.read_f32();
        for (int j = 0; j < 9; j++) p.rot[j] = r.read_f32();
        p.sx = r.read_f32(); p.sy = r.read_f32(); p.sz = r.read_f32();
        result.props.push_back(std::move(p));
    }

    spdlog::info("SCMAP: parsed {} props", result.props.size());
    return result;
}

} // namespace osc::map
