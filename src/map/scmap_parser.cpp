#include "map/scmap_parser.hpp"

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

    /// Read Int16 array of count elements.
    std::vector<i16> read_i16_array(size_t count) {
        std::vector<i16> result(count);
        std::memcpy(result.data(), data_ + pos_, count * 2);
        pos_ += count * 2;
        return result;
    }

    const u8* data_at(size_t offset) const { return data_ + offset; }

private:
    const u8* data_;
    size_t size_;
    size_t pos_;
};

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
    r.skip(static_cast<size_t>(preview_length));

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
    std::vector<i16> heightmap = r.read_i16_array(heightmap_count);

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

    // --- Done — skip rest of file (textures, decals, props) ---

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
    return result;
}

} // namespace osc::map
