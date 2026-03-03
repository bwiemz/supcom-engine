#include "sim/sca_parser.hpp"

#include <algorithm>
#include <cstring>

#include <spdlog/spdlog.h>

namespace osc::sim {

namespace {

/// Simple binary reader (same pattern as scm_parser.cpp).
class BinaryReader {
public:
    BinaryReader(const char* data, size_t size)
        : data_(data), size_(size), pos_(0) {}

    bool has_remaining(size_t bytes) const { return pos_ + bytes <= size_; }
    size_t position() const { return pos_; }
    size_t size() const { return size_; }

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
        if (!result.empty() && result.back().empty()) {
            result.pop_back();
        }
        size_t advance = std::min(length, size_ - pos_);
        pos_ += advance;
        return result;
    }

private:
    const char* data_;
    size_t size_;
    size_t pos_;
};

} // anonymous namespace

std::optional<SCAData> parse_sca(const std::vector<char>& file_data) {
    if (file_data.size() < 36) {
        spdlog::debug("SCA: file too small ({} bytes)", file_data.size());
        return std::nullopt;
    }

    BinaryReader reader(file_data.data(), file_data.size());

    // --- Header (36 bytes) ---
    // Magic "ANIM"
    char magic[4];
    std::memcpy(magic, file_data.data(), 4);
    reader.skip(4);
    if (magic[0] != 'A' || magic[1] != 'N' || magic[2] != 'I' ||
        magic[3] != 'M') {
        spdlog::debug("SCA: invalid magic");
        return std::nullopt;
    }

    u32 version = reader.read_u32();
    if (version != 5) {
        spdlog::debug("SCA: unsupported version {}", version);
        return std::nullopt;
    }

    u32 num_frames = reader.read_u32();
    f32 duration = reader.read_f32();
    u32 num_bones = reader.read_u32();
    u32 name_offset = reader.read_u32();
    u32 link_offset = reader.read_u32();
    u32 anim_offset = reader.read_u32();
    u32 frame_size = reader.read_u32();

    if (num_frames == 0 || num_bones == 0) {
        spdlog::debug("SCA: no frames ({}) or bones ({})", num_frames, num_bones);
        return std::nullopt;
    }

    u32 expected_frame_size = num_bones * 28 + 8;
    if (frame_size != expected_frame_size) {
        spdlog::debug("SCA: frame_size {} != expected {} (bones={})",
                       frame_size, expected_frame_size, num_bones);
        return std::nullopt;
    }

    // --- NAME section: bone names ---
    // Find the NAME tag and read bone name strings
    if (name_offset >= file_data.size()) {
        spdlog::debug("SCA: name_offset {} beyond file size", name_offset);
        return std::nullopt;
    }

    // The name_offset points past the "NAME" tag to the name data.
    // Name section ends at link_offset (or the "LINK" tag before it).
    size_t name_data_end = link_offset;
    // Walk back to skip the "LINK" tag (4 bytes) if present
    if (name_data_end > name_offset) {
        // Search backward for LINK tag — it's typically 4 bytes before
        // link_offset. The name data fills the space between name_offset and
        // the LINK tag.
        size_t name_section_length = name_data_end - name_offset;
        // Trim any trailing tag bytes — look for "LINK" at the end
        if (name_section_length >= 4) {
            const char* check = file_data.data() + name_data_end - 4;
            if (check[0] == 'L' && check[1] == 'I' && check[2] == 'N' &&
                check[3] == 'K') {
                name_section_length -= 4;
            }
        }
        reader.seek(name_offset);
        if (!reader.has_remaining(name_section_length)) {
            spdlog::debug("SCA: name section truncated");
            return std::nullopt;
        }
        // Read names
        auto names = reader.read_cstring_block(name_section_length);
        // Store names
        SCAData result;
        result.num_frames = num_frames;
        result.num_bones = num_bones;
        result.duration = duration;
        result.bone_names = std::move(names);

        // Pad bone_names to num_bones if fewer names parsed
        while (result.bone_names.size() < num_bones) {
            result.bone_names.push_back("bone_" +
                                         std::to_string(result.bone_names.size()));
        }

        // --- LINK section: parent indices ---
        reader.seek(link_offset);
        // If the "LINK" tag is present at link_offset, skip it
        if (reader.has_remaining(4)) {
            const char* tag = file_data.data() + link_offset;
            if (tag[0] == 'L' && tag[1] == 'I' && tag[2] == 'N' &&
                tag[3] == 'K') {
                reader.skip(4);
            }
        }
        if (!reader.has_remaining(num_bones * 4)) {
            spdlog::debug("SCA: link section truncated");
            return std::nullopt;
        }
        result.parent_indices.resize(num_bones);
        for (u32 i = 0; i < num_bones; i++) {
            result.parent_indices[i] = reader.read_i32();
        }

        // --- DATA section: animation frames ---
        // Compute where frames start: total data - frames data = prefix
        size_t total_data = file_data.size() - anim_offset;
        size_t frames_data = static_cast<size_t>(num_frames) * frame_size;
        if (frames_data > total_data) {
            spdlog::debug("SCA: frame data exceeds file (need {}, have {})",
                           frames_data, total_data);
            return std::nullopt;
        }
        size_t prefix_size = total_data - frames_data;
        size_t frames_start = anim_offset + prefix_size;

        reader.seek(frames_start);

        result.frames.resize(num_frames);
        for (u32 f = 0; f < num_frames; f++) {
            if (!reader.has_remaining(frame_size)) {
                spdlog::debug("SCA: frame {} truncated", f);
                return std::nullopt;
            }

            auto& frame = result.frames[f];
            frame.time = reader.read_f32();
            reader.skip(4); // flags

            frame.bones.resize(num_bones);
            for (u32 b = 0; b < num_bones; b++) {
                auto& bone = frame.bones[b];
                bone.position.x = reader.read_f32();
                bone.position.y = reader.read_f32();
                bone.position.z = reader.read_f32();
                // SCA stores quaternion as (w, x, y, z)
                // Our Quaternion is {x, y, z, w}
                f32 w = reader.read_f32();
                f32 x = reader.read_f32();
                f32 y = reader.read_f32();
                f32 z = reader.read_f32();
                bone.rotation = {x, y, z, w};
            }
        }

        // If frame times are all zero (some exporters omit timestamps),
        // distribute evenly across duration
        bool all_zero = true;
        for (auto& fr : result.frames) {
            if (fr.time > 0.0001f) { all_zero = false; break; }
        }
        if (all_zero && num_frames > 1) {
            f32 dt = duration / static_cast<f32>(num_frames - 1);
            for (u32 i = 0; i < num_frames; i++) {
                result.frames[i].time = static_cast<f32>(i) * dt;
            }
        }

        spdlog::debug("SCA: parsed {} frames, {} bones, {:.2f}s duration",
                       num_frames, num_bones, duration);
        return result;
    }

    spdlog::debug("SCA: name section empty or invalid");
    return std::nullopt;
}

} // namespace osc::sim
