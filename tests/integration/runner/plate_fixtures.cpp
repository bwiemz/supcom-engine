#include "plate_fixtures.hpp"

#include "integration_tests.hpp"
#include "lua/lua_state.hpp"
#include "sim/sim_state.hpp"

#include <spdlog/spdlog.h>

#include <initializer_list>

namespace osc::test {

void write_plate_scm(const std::filesystem::path& path, f32 half, u32 segments) {
    std::vector<char> d(48, 0);
    const auto put = [&](size_t offset, u32 v) { std::memcpy(d.data() + offset, &v, 4); };
    const auto append = [&](const void* p, size_t n) {
        const char* c = static_cast<const char*>(p);
        d.insert(d.end(), c, c + n);
    };
    const auto f = [&](std::initializer_list<f32> vs) {
        for (const f32 v : vs) append(&v, 4);
    };
    const auto u = [&](std::initializer_list<u32> vs) {
        for (const u32 v : vs) append(&v, 4);
    };
    std::memcpy(d.data(), "MODL", 4);
    put(4, 5); // version
    append("NAME", 4);
    append("root", 5); // and its terminator
    d.resize(60, 0);
    append("SKEL", 4);
    const auto bone_offset = static_cast<u32>(d.size());
    f({1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}); // rest pose: identity
    f({0, 0, 0});                                        // position
    f({1, 0, 0, 0});                                     // rotation (w, x, y, z)
    u({52, 0xFFFFFFFFu, 0, 0});                          // name, no parent
    d.resize(188, 0);
    append("VTXL", 4);
    const auto vert_offset = static_cast<u32>(d.size());
    const u32 side = segments + 1;
    for (u32 j = 0; j < side; ++j) {
        for (u32 i = 0; i < side; ++i) {
            const f32 u_at = static_cast<f32>(i) / static_cast<f32>(segments);
            const f32 v_at = static_cast<f32>(j) / static_cast<f32>(segments);
            f({-half + 2.0f * half * u_at, 0, -half + 2.0f * half * v_at}); // position
            f({0, 1, 0});                                                   // normal
            f({1, 0, 0});                                                   // tangent: along u
            f({0, 0, 1});                                                   // binormal: along v
            f({u_at, v_at});                                                // uv
            f({0, 0});                                                      // second uv
            u({0});                                                         // bones
        }
    }
    const auto index_offset = static_cast<u32>(d.size());
    std::vector<u16> indices;
    for (u32 j = 0; j < segments; ++j) {
        for (u32 i = 0; i < segments; ++i) {
            const auto a = static_cast<u16>(j * side + i);
            const auto b = static_cast<u16>(a + 1);
            const auto c = static_cast<u16>(a + side + 1);
            const auto e = static_cast<u16>(a + side);
            indices.insert(indices.end(), {a, b, c, a, c, e, a, c, b, a, e, c});
        }
    }
    append(indices.data(), indices.size() * sizeof(u16));
    put(8, bone_offset);
    put(12, 1); // bones the vertices use
    put(16, vert_offset);
    put(24, side * side);
    put(28, index_offset);
    put(32, static_cast<u32>(indices.size()));
    put(44, 1); // bones in all
    std::ofstream(path, std::ios::binary).write(d.data(), static_cast<std::streamsize>(d.size()));
}

void stand_plate(TestContext& ctx, const std::string& root, const std::string& bp,
                 const Plate& plate, f32 x, f32 z, f32 ground_y, bool prop, f32 lift) {
    std::string key = bp;
    for (char& c : key)
        if (c == '/' || c == '.') c = '_';
    const auto file = [&](const std::string& name) {
        return name.empty() ? std::string("nil") : fmt::format("'{}/{}'", root, name);
    };
    const std::string create =
        prop ? fmt::format("CreatePropHPR('{}', {}, {}, {}, 0, 0, 0)", bp, x, ground_y, z)
             : fmt::format("CreateUnitHPR('{}', 'ARMY_1', {}, 0, {}, 0, 0, 0)", bp, x, z);
    const std::string lua = fmt::format("local mesh = '{12}/{0}_plate'\n"
                                        "__blueprints[mesh] = {{ BlueprintId = mesh, LODs = {{ {{\n"
                                        "  LODCutoff = 1000, ShaderName = '{1}',\n"
                                        "  MeshName = '{12}/{11}',\n"
                                        "  AlbedoName = '{12}/{2}',\n"
                                        "  SpecularName = '{12}/{10}',\n"
                                        "  NormalsName = '{12}/{3}', LookupName = {4},\n"
                                        "  SecondaryName = {13} }} }} }}\n"
                                        "local bp = __blueprints['{5}']\n"
                                        "bp.Display = bp.Display or {{}}\n"
                                        "bp.Display.MeshBlueprint = mesh\n"
                                        "bp.Display.UniformScale = 1\n"
                                        "__osc_last_plate = {6}\n"
                                        "Warp(__osc_last_plate, Vector({7}, {8}, {9}))\n",
                                        key, plate.shader, plate.albedo, plate.normals,
                                        file(plate.lookup), bp, create, x, ground_y + 0.5f + lift,
                                        z, plate.specteam, plate.mesh, root, file(plate.secondary));
    const auto made = ctx.lua_state.do_string(lua);
    if (!made) spdlog::warn("the plate: {}", made.error().message);
    ctx.sim.tick();
}

} // namespace osc::test
