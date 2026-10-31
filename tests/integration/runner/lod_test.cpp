// --lod-test: a mesh draws the LOD Moho's LOD metric picks (Mesh::ComputeLOD), none past its
// last, and a later LOD skins onto the first's bones by name (MeshBatch::Initialize). The
// Megalith's two LODs (/units/xrl0404's meshes) order their bones differently; the civilian
// truck's (ope6003) have one bone.

#include "integration_tests.hpp"
#include "render_probe.hpp"
#include "tick_until.hpp"

#include "lua/lua_state.hpp"
#include "map/terrain.hpp"
#include "renderer/camera.hpp"
#include "renderer/command_feedback.hpp"
#include "renderer/mesh_cache.hpp"
#include "sim/scm_parser.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "vfs/virtual_file_system.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace osc::test {

namespace {

struct Scm {
    sim::SCMMesh mesh;
    std::vector<std::string> bones;
};

std::optional<Scm> read_scm(vfs::VirtualFileSystem& vfs, const std::string& path) {
    const auto data = vfs.read_file(path);
    if (!data) {
        return std::nullopt;
    }
    auto mesh = sim::parse_scm_mesh(*data);
    const auto bones = sim::parse_scm_bones(*data);
    if (!mesh || !bones) {
        return std::nullopt;
    }
    Scm scm{std::move(*mesh), {}};
    for (const sim::BoneInfo& bone : bones->bones) {
        scm.bones.push_back(bone.name);
    }
    return scm;
}

/// Each vertex moved by its first bone's skinning matrix (column-major).
std::vector<std::array<f32, 3>> skinned(const sim::SCMMesh& mesh,
                                        const std::vector<std::array<f32, 16>>& matrices) {
    std::vector<std::array<f32, 3>> out;
    out.reserve(mesh.vertices.size());
    for (const sim::SCMMesh::Vertex& v : mesh.vertices) {
        const u8 bone = v.bone_indices[0];
        if (bone >= matrices.size()) {
            out.push_back({v.px, v.py, v.pz});
            continue;
        }
        const std::array<f32, 16>& m = matrices[bone];
        out.push_back({m[0] * v.px + m[4] * v.py + m[8] * v.pz + m[12],
                       m[1] * v.px + m[5] * v.py + m[9] * v.pz + m[13],
                       m[2] * v.px + m[6] * v.py + m[10] * v.pz + m[14]});
    }
    return out;
}

/// The mean, over `points`, of the distance to the nearest of `surface`.
f32 mean_nearest(const std::vector<std::array<f32, 3>>& points,
                 const std::vector<std::array<f32, 3>>& surface) {
    if (points.empty() || surface.empty()) {
        return std::numeric_limits<f32>::infinity();
    }
    f64 total = 0;
    for (const auto& p : points) {
        f32 best = std::numeric_limits<f32>::infinity();
        for (const auto& s : surface) {
            const f32 dx = p[0] - s[0];
            const f32 dy = p[1] - s[1];
            const f32 dz = p[2] - s[2];
            best = std::min(best, dx * dx + dy * dy + dz * dz);
        }
        total += std::sqrt(best);
    }
    return static_cast<f32>(total / static_cast<f64>(points.size()));
}

/// The textures of the meshes the last frame drew (its dump's "mesh <texture> |" lines),
/// lowercased.
std::vector<std::string> drawn_textures(renderer::Renderer& renderer, size_t& cubes) {
    std::ostringstream out;
    renderer.dump_frame(out);
    std::istringstream in(out.str());
    std::vector<std::string> textures;
    cubes = 0;
    bool in_units = false;
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line[0] == '[') {
            in_units = line == "[units]";
            continue;
        }
        if (!in_units) {
            continue;
        }
        if (line.rfind("cube ", 0) == 0) {
            ++cubes;
        }
        if (line.rfind("mesh ", 0) == 0) {
            std::string texture = line.substr(5, line.find(" |") - 5);
            std::transform(texture.begin(), texture.end(), texture.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            textures.push_back(std::move(texture));
        }
    }
    return textures;
}

/// Which of a mesh's LODs the frame drew: "lod0", "lod1" or "none", by its textures (`base`
/// names both; the second's has "_lod1_").
std::string drawn_lod(const std::vector<std::string>& textures, const std::string& base) {
    for (const std::string& t : textures) {
        if (t.find(base) != std::string::npos) {
            return t.find("_lod1_") != std::string::npos ? "lod1" : "lod0";
        }
    }
    return "none";
}

} // namespace

void test_lod(TestContext& ctx) {
    spdlog::info("=== LOD TEST: Moho's LOD choice and an LOD's bones ===");
    Tally t;
    const auto made = ctx.lua_state.do_string(R"(
        local h = GetTerrainHeight
        __osc_lod_mega = CreateUnitHPR('xrl0403', 'ARMY_1', 620, h(620, 90), 90, 0, 0, 0)
        __osc_lod_truck = CreateUnitHPR('ope6003', 'ARMY_1', 632, h(632, 90), 90, 0, 0, 0)
        IssueMove({__osc_lod_mega}, {620, h(620, 150), 150})
    )");
    t.check(static_cast<bool>(made),
            made ? std::string("the Megalith and the truck made") : made.error().message);
    if (!made) {
        return;
    }
    tick_for(ctx, 30);
    const sim::Unit* mega = nullptr;
    const sim::Unit* truck = nullptr;
    ctx.sim.entity_registry().for_each_unit([&](sim::Entity& e) {
        if (e.blueprint_id() == "xrl0403") {
            mega = static_cast<const sim::Unit*>(&e);
        } else if (e.blueprint_id() == "ope6003") {
            truck = static_cast<const sim::Unit*>(&e);
        }
    });
    if (!mega || !truck) {
        t.check(false, "the Megalith and the truck found");
        return;
    }

    // Test 1: the Megalith's second LOD, skinned by its walking pose through the remap, lies as
    // near its first LOD's surface as at rest.
    {
        const auto lod0 = read_scm(ctx.vfs, "/units/xrl0404/xrl0404_lod0.scm");
        const auto lod1 = read_scm(ctx.vfs, "/units/xrl0404/xrl0404_lod1.scm");
        const auto& matrices = mega->animated_bone_matrices();
        if (!lod0 || !lod1 || matrices.size() != lod0->bones.size()) {
            t.check(false, fmt::format("Test 1: the Megalith's meshes and pose ({} bones)",
                                       matrices.size()));
        } else {
            sim::SCMMesh remapped = lod1->mesh;
            renderer::remap_vertex_bones(remapped,
                                         renderer::lod_bone_remap(lod1->bones, lod0->bones));
            const std::vector<std::array<f32, 16>> rest(
                matrices.size(), {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1});
            const auto surface = skinned(lod0->mesh, matrices);
            const f32 at_rest = mean_nearest(skinned(lod1->mesh, rest), skinned(lod0->mesh, rest));
            const f32 apart = mean_nearest(skinned(remapped, matrices), surface);
            const f32 unmapped = mean_nearest(skinned(lod1->mesh, matrices), surface);
            const f32 moved = mean_nearest(surface, skinned(lod0->mesh, rest));
            t.check(moved > 2.0f * at_rest && apart < 1.5f * at_rest,
                    fmt::format("Test 1: the Megalith's second LOD, skinned onto its first's "
                                "bones, lies {:.3f} from its first's surface, {:.3f} at rest "
                                "({:.3f} without the remap; its pose moves its vertices {:.3f})",
                                apart, at_rest, unmapped, moved));
        }
    }

    // Test 2: each draws the LOD its metric picks, none past its last cutoff: the truck's are
    // 70 and 215, the Megalith's 200 and 400. (Past an eye 250 away the view is strategic and
    // draws no mesh.)
    OffscreenShots shots(ctx);
    if (!shots.ok()) {
        t.check(false, "Test 2: renderer init (no Vulkan?)");
        return;
    }
    renderer::Camera& camera = shots.renderer().camera();
    const f32 per_depth = 2.0f * std::tan(camera.fov() * 0.5f);
    struct Band {
        f32 metric;
        const char* mega;
        const char* truck;
    };
    for (const Band band :
         {Band{60, "lod0", "lod0"}, Band{150, "lod0", "lod1"}, Band{230, "lod1", "none"}}) {
        const sim::Vector3 at = mega->position();
        (void)shots.shoot_frame(*ctx.sim.terrain(), at.x + 6.0f, at.z, band.metric / per_depth);
        sim::Vector3 eye;
        camera.eye_position(eye.x, eye.y, eye.z);
        const std::array<f32, 3> d = camera.direction();
        const sim::Vector3 forward{d[0], d[1], d[2]};
        size_t cubes = 0;
        const auto textures = drawn_textures(shots.renderer(), cubes);
        const std::string mega_lod = drawn_lod(textures, "/units/xrl0403/");
        const std::string truck_lod = drawn_lod(textures, "/units/uec0001/");
        t.check(mega_lod == band.mega && truck_lod == band.truck && cubes == 0,
                fmt::format("Test 2: at a metric of {:.0f} the Megalith ({:.0f}) draws {}, want "
                            "{}; the truck ({:.0f}) {}, want {}; {} cubes",
                            band.metric,
                            renderer::lod_metric(mega->position(), eye, forward, camera.fov()),
                            mega_lod, band.mega,
                            renderer::lod_metric(truck->position(), eye, forward, camera.fov()),
                            truck_lod, band.truck, cubes));
    }

    spdlog::info("LOD test: {}/{} passed", t.pass, t.pass + t.fail);
}

} // namespace osc::test
