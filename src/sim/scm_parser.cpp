#include "sim/scm_parser.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>

#include <spdlog/spdlog.h>

namespace osc::sim {

namespace {

/// Simple binary reader (same pattern as scmap_parser.cpp).
class BinaryReader {
public:
    BinaryReader(const char* data, size_t size)
        : data_(data), size_(size), pos_(0) {}

    bool has_remaining(size_t bytes) const { return pos_ + bytes <= size_; }
    size_t position() const { return pos_; }

    u32 read_u32() {
        u32 val;
        std::memcpy(&val, data_ + pos_, 4);
        pos_ += 4;
        return val;
    }

    i32 read_i32() {
        i32 val;
        std::memcpy(&val, data_ + pos_, 4);
        pos_ += 4;
        return val;
    }

    f32 read_f32() {
        f32 val;
        std::memcpy(&val, data_ + pos_, 4);
        pos_ += 4;
        return val;
    }

    void skip(size_t bytes) { pos_ += bytes; }

    void seek(size_t offset) { pos_ = offset; }

    /// Read a block of null-terminated strings, splitting on '\0'.
    /// Keeps empty strings to preserve index alignment with bone entries.
    /// Strips only the trailing empty entry (matching Python: split(b'\0')[:-1]).
    std::vector<std::string> read_cstring_block(size_t length) {
        std::vector<std::string> result;
        std::string current;
        for (size_t i = 0; i < length && pos_ + i < size_; i++) {
            char c = data_[pos_ + i];
            if (c == '\0') {
                result.push_back(std::move(current));
                current.clear();
            } else {
                current.push_back(c);
            }
        }
        // Strip trailing empty entry (the block ends with \0 producing one
        // extra empty entry)
        if (!result.empty() && result.back().empty()) {
            result.pop_back();
        }
        pos_ += length;
        return result;
    }

private:
    const char* data_;
    size_t size_;
    size_t pos_;
};

} // anonymous namespace

std::optional<BoneData> parse_scm_bones(const std::vector<char>& file_data) {
    if (file_data.size() < 48) {
        spdlog::debug("SCM: file too small ({} bytes)", file_data.size());
        return std::nullopt;
    }

    BinaryReader reader(file_data.data(), file_data.size());

    // --- Header (48 bytes): '4s11I' ---
    // Magic "MODL"
    char magic[4];
    std::memcpy(magic, file_data.data(), 4);
    reader.skip(4);
    if (magic[0] != 'M' || magic[1] != 'O' || magic[2] != 'D' ||
        magic[3] != 'L') {
        spdlog::debug("SCM: invalid magic");
        return std::nullopt;
    }

    u32 version = reader.read_u32();
    if (version != 5) {
        spdlog::debug("SCM: unsupported version {}", version);
        return std::nullopt;
    }

    u32 bone_offset = reader.read_u32();
    u32 bone_count = reader.read_u32();

    // Skip remaining header fields (vert_offset through total_bone_count)
    // We only need bone_offset and bone_count
    reader.skip(7 * 4); // 7 more u32s to reach byte 48

    if (bone_count == 0) {
        spdlog::debug("SCM: no bones");
        return std::nullopt;
    }

    // Validate bone_offset is within file
    if (bone_offset >= file_data.size()) {
        spdlog::debug("SCM: bone_offset {} beyond file size {}",
                       bone_offset, file_data.size());
        return std::nullopt;
    }

    // --- Bone names ---
    // Null-terminated strings between byte 48 and (bone_offset - 4).
    // The Python parser uses: length = (boneoffset - 4) - scm.tell()
    // The last 4 bytes before bone data are padding/marker, not names.
    size_t name_section_start = reader.position(); // should be 48
    size_t name_section_length = (bone_offset >= name_section_start + 4)
        ? (bone_offset - 4) - name_section_start
        : bone_offset - name_section_start;
    if (!reader.has_remaining(name_section_length)) {
        spdlog::debug("SCM: name section truncated");
        return std::nullopt;
    }
    auto names = reader.read_cstring_block(name_section_length);

    // --- Bone data (108 bytes each at bone_offset) ---
    // Format: 16f (64B matrix) + 3f (12B pos) + 4f (16B quat) + 4i (16B ints)
    // Total = 108 bytes per bone
    static constexpr size_t BONE_ENTRY_SIZE = 108;
    reader.seek(bone_offset);

    if (!reader.has_remaining(bone_count * BONE_ENTRY_SIZE)) {
        spdlog::debug("SCM: bone data truncated (need {} bytes from offset {})",
                       bone_count * BONE_ENTRY_SIZE, bone_offset);
        return std::nullopt;
    }

    BoneData result;
    result.bones.resize(bone_count);

    for (u32 i = 0; i < bone_count; i++) {
        auto& bone = result.bones[i];

        // Bone name from names section (may have fewer names than bones)
        if (i < names.size()) {
            bone.name = names[i];
        } else {
            bone.name = "bone_" + std::to_string(i);
        }

        // Read 4x4 rest_pose_inverse matrix (64 bytes, row-major in file)
        // Transpose to column-major for our convention
        for (int row = 0; row < 4; row++)
            for (int col = 0; col < 4; col++)
                bone.inverse_bind_pose[col * 4 + row] = reader.read_f32();

        // Position relative to parent (3 floats)
        bone.local_position.x = reader.read_f32();
        bone.local_position.y = reader.read_f32();
        bone.local_position.z = reader.read_f32();

        // Rotation quaternion relative to parent (x, y, z, w)
        bone.local_rotation.x = reader.read_f32();
        bone.local_rotation.y = reader.read_f32();
        bone.local_rotation.z = reader.read_f32();
        bone.local_rotation.w = reader.read_f32();

        // Parent bone index (i32, -1 = root)
        bone.parent_index = reader.read_i32();

        // Skip 3 unused ints (12 bytes)
        reader.skip(12);
    }

    // Build name_to_index map (lowercase keys for case-insensitive lookup)
    for (i32 i = 0; i < static_cast<i32>(result.bones.size()); i++) {
        std::string lower = result.bones[i].name;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        result.name_to_index[lower] = i;
    }

    // Pre-compute world positions and rotations by walking parent chain.
    // Bones are stored such that parent_index < current index (parents come
    // first), so a single forward pass suffices.
    for (u32 i = 0; i < bone_count; i++) {
        auto& bone = result.bones[i];
        if (bone.parent_index < 0 ||
            bone.parent_index >= static_cast<i32>(bone_count)) {
            // Root bone: world = local
            bone.world_position = bone.local_position;
            bone.world_rotation = bone.local_rotation;
        } else {
            auto& parent = result.bones[bone.parent_index];
            // Accumulate rotation: world_rot = parent_world_rot * local_rot
            bone.world_rotation = quat_multiply(parent.world_rotation,
                                                 bone.local_rotation);
            // World position = parent world pos + rotate(local_pos,
            // parent_accumulated_rotation)
            auto rotated = quat_rotate(parent.world_rotation,
                                        bone.local_position);
            bone.world_position = {
                parent.world_position.x + rotated.x,
                parent.world_position.y + rotated.y,
                parent.world_position.z + rotated.z};
        }
    }

    spdlog::debug("SCM: parsed {} bones", bone_count);
    return result;
}

std::optional<SCMMesh> parse_scm_mesh(const std::vector<char>& file_data) {
    if (file_data.size() < 48) {
        spdlog::debug("SCM mesh: file too small ({} bytes)", file_data.size());
        return std::nullopt;
    }

    BinaryReader reader(file_data.data(), file_data.size());

    // --- Header (48 bytes): '4s11I' ---
    char magic[4];
    std::memcpy(magic, file_data.data(), 4);
    reader.skip(4);
    if (magic[0] != 'M' || magic[1] != 'O' || magic[2] != 'D' ||
        magic[3] != 'L') {
        spdlog::debug("SCM mesh: invalid magic");
        return std::nullopt;
    }

    u32 version = reader.read_u32();
    if (version != 5) {
        spdlog::debug("SCM mesh: unsupported version {}", version);
        return std::nullopt;
    }

    // Header fields: bone_offset, bone_count, vert_offset, extra_vert_offset,
    //                vert_count, index_offset, index_count, info_offset,
    //                info_count, total_bone_count
    reader.skip(4); // bone_offset
    reader.skip(4); // bone_count
    u32 vert_offset = reader.read_u32();
    reader.skip(4); // extra_vert_offset
    u32 vert_count = reader.read_u32();
    u32 index_offset = reader.read_u32();
    u32 index_count = reader.read_u32();
    // Skip remaining: info_offset, info_count, total_bone_count
    // reader is now at byte 48

    if (vert_count == 0 || index_count == 0) {
        spdlog::debug("SCM mesh: no vertices or indices");
        return std::nullopt;
    }

    // --- Vertices (68 bytes each) ---
    // Layout: 3f pos, 3f tangent, 3f normal, 3f binormal, 2f uv1, 2f uv2, 4B bone
    static constexpr size_t VERT_SIZE = 68;
    if (vert_offset + static_cast<size_t>(vert_count) * VERT_SIZE > file_data.size()) {
        spdlog::debug("SCM mesh: vertex data truncated");
        return std::nullopt;
    }

    SCMMesh mesh;
    mesh.vertices.resize(vert_count);
    reader.seek(vert_offset);

    for (u32 i = 0; i < vert_count; i++) {
        auto& v = mesh.vertices[i];
        // Position (3 floats)
        v.px = reader.read_f32();
        v.py = reader.read_f32();
        v.pz = reader.read_f32();
        // Skip tangent (3 floats = 12 bytes)
        reader.skip(12);
        // Normal (3 floats)
        v.nx = reader.read_f32();
        v.ny = reader.read_f32();
        v.nz = reader.read_f32();
        // Skip binormal (3 floats = 12 bytes)
        reader.skip(12);
        // UV1 (2 floats)
        v.u = reader.read_f32();
        v.v = reader.read_f32();
        // Skip UV2 (2 floats = 8 bytes)
        reader.skip(8);
        // Bone index (4 bytes — rigid skinning: first byte is bone index)
        u8 bone_bytes[4];
        std::memcpy(bone_bytes, file_data.data() + reader.position(), 4);
        reader.skip(4);
        v.bone_index = static_cast<i32>(bone_bytes[0]);
    }

    // --- Indices (6 bytes per triangle = 3 × u16) ---
    // index_count is the total number of indices (not triangles)
    static constexpr size_t INDEX_SIZE = 2; // u16 per index
    if (index_offset + static_cast<size_t>(index_count) * INDEX_SIZE > file_data.size()) {
        spdlog::debug("SCM mesh: index data truncated");
        return std::nullopt;
    }

    mesh.indices.resize(index_count);
    reader.seek(index_offset);

    for (u32 i = 0; i < index_count; i++) {
        u16 idx;
        std::memcpy(&idx, file_data.data() + reader.position(), 2);
        reader.skip(2);
        mesh.indices[i] = static_cast<u32>(idx);
    }

    spdlog::debug("SCM mesh: parsed {} vertices, {} indices ({} triangles)",
                   vert_count, index_count, index_count / 3);
    return mesh;
}

} // namespace osc::sim
