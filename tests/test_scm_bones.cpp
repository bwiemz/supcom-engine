#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "sim/scm_parser.hpp"

#include <cstring>
#include <string>
#include <vector>

namespace {

struct Writer {
    std::vector<char> bytes;
    void raw(const void* p, size_t n) {
        const auto* c = static_cast<const char*>(p);
        bytes.insert(bytes.end(), c, c + n);
    }
    void u32(unsigned v) { raw(&v, 4); }
    void i32(int v) { raw(&v, 4); }
    void f32(float v) { raw(&v, 4); }
    void pad_to(size_t n) { bytes.resize(n, static_cast<char>(0xC5)); }
};

struct TestBone {
    const char* name;
    float px, py, pz;
    float qw, qx, qy, qz; // as SCM stores it: w first
    int parent;
};

/// An SCM v5 file holding just a skeleton: header, NAME block, SKEL block.
std::vector<char> scm_with(const std::vector<TestBone>& bones) {
    constexpr unsigned kBoneOffset = 96;
    Writer w;
    w.raw("MODL", 4);
    w.u32(5);
    w.u32(kBoneOffset);
    w.u32(static_cast<unsigned>(bones.size())); // weighted bones
    for (int i = 0; i < 7; ++i) w.u32(0);
    w.u32(static_cast<unsigned>(bones.size())); // total bones
    w.raw("NAME", 4);
    std::vector<unsigned> name_offsets;
    for (const auto& b : bones) {
        name_offsets.push_back(static_cast<unsigned>(w.bytes.size()));
        w.raw(b.name, std::strlen(b.name) + 1);
    }
    w.pad_to(kBoneOffset - 4);
    w.raw("SKEL", 4);
    for (size_t i = 0; i < bones.size(); ++i) {
        const auto& b = bones[i];
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) w.f32(r == c ? 1.0f : 0.0f); // inverse bind
        w.f32(b.px);
        w.f32(b.py);
        w.f32(b.pz);
        w.f32(b.qw);
        w.f32(b.qx);
        w.f32(b.qy);
        w.f32(b.qz);
        w.u32(name_offsets[i]);
        w.i32(b.parent);
        w.i32(0);
        w.i32(0);
    }
    return w.bytes;
}

} // namespace

TEST_CASE("SCM bones: rotation is stored w first, then the name offset, then the parent", "[scm]") {
    const float h = 0.70710678f; // 90 degrees about Y
    const auto data = scm_with({
        {"root", 0, 0, 0, 1, 0, 0, 0, -1},
        {"child", 0, 0, 2, h, 0, h, 0, 0},
        {"tip", 0, 0, 1, 1, 0, 0, 0, 1},
    });
    const auto parsed = osc::sim::parse_scm_bones(data);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->bones.size() == 3);
    const auto& bones = parsed->bones;

    CHECK(bones[0].name == "root");
    CHECK(bones[2].name == "tip");
    CHECK(bones[0].parent_index == -1);
    CHECK(bones[1].parent_index == 0);
    CHECK(bones[2].parent_index == 1);

    CHECK(bones[0].local_rotation.w == Catch::Approx(1.0f));
    CHECK(bones[0].local_rotation.x == Catch::Approx(0.0f));
    CHECK(bones[1].local_rotation.y == Catch::Approx(h));
    CHECK(bones[1].local_rotation.w == Catch::Approx(h));

    // The tip sits one unit ahead of the child, which is turned to face +X.
    CHECK(bones[2].world_position.x == Catch::Approx(1.0f).margin(1e-5));
    CHECK(bones[2].world_position.y == Catch::Approx(0.0f).margin(1e-5));
    CHECK(bones[2].world_position.z == Catch::Approx(2.0f).margin(1e-5));
}
