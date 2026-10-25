// --dispatch-matrix-test: actual mesh dispatch, across every technique and view.
#include "integration_tests.hpp"
#include "plate_fixtures.hpp"
#include "render_probe.hpp"
#include "support/temp_path.hpp"

#include "lua/lua_state.hpp"
#include "blueprints/blueprint_store.hpp"
#include "core/ui_registry_keys.hpp"
#include "map/terrain.hpp"
#include "sim/sim_state.hpp"
#include "ui/ui_control.hpp"
#include "ui/world_view.hpp"
#include "vfs/directory_mount.hpp"
#include "vfs/virtual_file_system.hpp"

#include <lua.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>

namespace osc::test {
namespace {
std::vector<std::string> decisions(renderer::Renderer& r) {
    std::ostringstream dump;
    r.dump_frame(dump);
    std::vector<std::string> lines;
    std::istringstream input(dump.str());
    for (std::string line; std::getline(input, line);)
        if (line.find("technique=") != std::string::npos) lines.push_back(line);
    std::sort(lines.begin(), lines.end());
    return lines;
}
} // namespace

void test_dispatch_matrix(TestContext& ctx, TestContext& ui) {
    Tally t;
    OffscreenShots shots(ctx);
    if (!shots.ok()) {
        t.check(false, "dispatch matrix: Vulkan renderer");
        return;
    }
    auto& r = shots.renderer();
    using renderer::MeshTechnique;
    const TempDir scratch("osc_dispatch_matrix");
    const auto& dir = scratch.path();
    write_plate_scm(dir / "plate.scm", 4.0f);
    const auto flat = [](int, u32, u32) { return std::array<u8, 4>{128, 128, 128, 128}; };
    for (const char* file : {"albedo.dds", "second.dds", "plate_normals.dds", "plate_specteam.dds"})
        write_dds(dir / file, 1, flat);
    constexpr const char* root = "/osc_dispatch_matrix";
    ctx.vfs.mount(root, std::make_unique<vfs::DirectoryMount>(dir));
    map::Terrain ground(map::Heightmap(512, 512, 1.0f, std::vector<u16>(513 * 513, 0)), 0.0f, true);
    ground.set_lighting(white_fill(), {});
    constexpr u32 techniques = static_cast<u32>(MeshTechnique::AeonCZAR) + 1;
    bool shared = true;
    for (u32 id = 0; id <= techniques; ++id) {
        if (id == static_cast<u32>(MeshTechnique::UnitPlace)) continue;
        Plate plate;
        plate.shader = renderer::mesh_technique_name(
            id < techniques ? static_cast<MeshTechnique>(id) : MeshTechnique::Unit);
        plate.albedo = id < techniques ? "albedo.dds" : "second.dds";
        const char* bp = kPlateBlueprints[id];
        const f32 at = id < techniques ? 128.0f : 400.0f;
        stand_plate(ctx, root, bp, plate, at, at, 0.0f);
        if (id == static_cast<u32>(MeshTechnique::UEFBuild))
            stand_plate(ctx, root, bp, plate, at, at, 0.0f, false, 0.25f);
        lua_getglobal(ctx.L, "__blueprints");
        lua_pushstring(ctx.L, fmt::format("{}/{}_plate", root, bp).c_str());
        lua_rawget(ctx.L, -2);
        ctx.store.register_blueprint(ctx.L, blueprints::BlueprintType::Mesh, lua_gettop(ctx.L));
        lua_pop(ctx.L, 2);
        // Both Lua states resolve the same test material, including the minimap's LODs.
        const auto copied = ui.lua_state.do_string(fmt::format(
            "local mesh = '{0}/{1}_plate'\n"
            "__blueprints['{1}'].Display.MeshBlueprint = mesh\n"
            "__blueprints[mesh] = {{ BlueprintId = mesh, IconFadeInZoom = 130, "
            "LODs = {{ {{ LODCutoff = 1000, ShaderName = '{2}', MeshName = '{0}/plate.scm', "
            "AlbedoName = '{0}/{3}', NormalsName = '{0}/plate_normals.dds', "
            "SpecularName = '{0}/plate_specteam.dds' }} }} }}\n",
            root, bp, plate.shader, plate.albedo));
        shared &= static_cast<bool>(copied);
    }
    renderer::BuildGhost ghost;
    ghost.blueprint_id = kPlateBlueprints[0];
    ghost.x = ghost.z = 128.0f;
    ghost.y = 0.5f;
    t.check(shared, "dispatch matrix: materials available in sim and UI states");
    shots.recapture();
    r.set_record_dispatch(true);
    (void)shots.shoot_frame(ground, 128.0f, 128.0f, 40.0f);

    // Change only the order of two overlapping translucent groups.
    renderer::GPUMesh* build = nullptr;
    renderer::GPUMesh* cube = nullptr;
    for (const auto& g : r.unit_renderer().mesh_groups()) {
        if (g.mesh->technique == MeshTechnique::UEFBuild)
            build = const_cast<renderer::GPUMesh*>(g.mesh);
        if (g.mesh->technique == MeshTechnique::UEFBuildCube)
            cube = const_cast<renderer::GPUMesh*>(g.mesh);
    }
    if (build && cube) {
        build->sort_order = -10;
        cube->sort_order = -9;
        shots.redraw(nullptr, &ghost);
        const auto before = decisions(r);
        std::swap(build->sort_order, cube->sort_order);
        shots.redraw(nullptr, &ghost);
        t.check(before != decisions(r), "dispatch order: reversed translucent groups change dump");
        build->sort_order = cube->sort_order = 0;
    } else t.check(false, "dispatch order: both translucent groups are present");

    lua_State* L = ui.L;
    lua_pushstring(L, "osc_ui_registry");
    lua_rawget(L, LUA_REGISTRYINDEX);
    auto* registry = static_cast<ui::UIControlRegistry*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    if (!registry) {
        t.check(false, "dispatch matrix: UI registry");
        return;
    }
    lua_pushstring(L, "__osc_renderer");
    lua_pushlightuserdata(L, &r);
    lua_rawset(L, LUA_REGISTRYINDEX);
    lua_pushstring(L, core::kWorldUiActiveKey);
    lua_pushboolean(L, 1);
    lua_rawset(L, LUA_REGISTRYINDEX);
    r.init_ui_caches(&ctx.vfs);
    ui::WorldView::set_default_mini_lod(0.01f);
    const auto made = ui.lua_state.do_string(
        "for i = 0, 2 do\n"
        " local v = {} setmetatable(v, {__index = moho.UIWorldView})\n"
        " moho.UIWorldView.__init(v, GetFrame(0), i == 2 and 'MiniMap' or "
        "(i == 0 and 'WorldCamera' or 'WorldCamera2'), 1, i == 2, 'WorldCamera')\n"
        " v.Left:Set(i == 1 and 400 or 0) v.Top:Set(0)\n"
        " v.Width:Set(i == 2 and 200 or 400) v.Height:Set(i == 2 and 150 or 600)\n"
        " v.Right:Set(v.Left() + v.Width()) v.Bottom:Set(v.Height())\n"
        " if i == 2 then moho.UIWorldView.SetCartographic(v, true) end\n"
        " rawset(_G, '__osc_matrix_view' .. i, v)\n"
        "end\n");
    t.check(static_cast<bool>(made), "dispatch matrix: two main views and a minimap");
    shots.draw_ui(ui.L, registry);
    auto& camera2 = r.add_camera("WorldCamera2");
    camera2.set_free(true);
    camera2.set_pitch(OffscreenShots::kPitch);
    camera2.set_eye_distance(40.0f);
    camera2.set_target(400.0f, 400.0f);
    std::vector<std::string> matrix;
    for (int lane = 0; lane < 3; ++lane) {
        r.video_options().graphics_fidelity = lane;
        for (bool cartographic : {false, true}) {
            r.camera().set_ortho(cartographic);
            camera2.set_ortho(cartographic);
            shots.redraw(nullptr, &ghost);
            shots.redraw(nullptr, &ghost);
            t.check(r.drawn_views().size() == 2 && r.minimap_meshes_gathered(),
                    "dispatch matrix: two active views and minimap gathered");
            const auto lines = decisions(r);
            bool first = false;
            bool other = false;
            bool aligned = true;
            for (const auto& line : lines) {
                if (line.find("view=0") != std::string::npos &&
                    line.find("mesh /osc_dispatch_matrix/") != std::string::npos &&
                    line.find("technique=UnitPlace ") == std::string::npos)
                    aligned &= line.find("albedo.dds") != std::string::npos &&
                               (line.find("128.0000 0.5000 128.0000 1.0000") != std::string::npos ||
                                line.find("128.0000 0.7500 128.0000 1.0000") != std::string::npos);
                if (line.find("view=1") != std::string::npos &&
                    line.find("mesh /osc_dispatch_matrix/") != std::string::npos &&
                    line.find("technique=UnitPlace ") == std::string::npos)
                    aligned &= line.find("second.dds") != std::string::npos &&
                               line.find("400.0000 0.5000 400.0000 1.0000") != std::string::npos;
                first |= line.find("view=0") != std::string::npos &&
                         line.find("mesh /osc_dispatch_matrix/albedo.dds") != std::string::npos &&
                         line.find("128.0000 0.5000 128.0000 1.0000") != std::string::npos;
                other |= line.find("view=1") != std::string::npos &&
                         line.find("mesh /osc_dispatch_matrix/second.dds") != std::string::npos &&
                         line.find("400.0000 0.5000 400.0000 1.0000") != std::string::npos;
                matrix.push_back(fmt::format("fidelity={} mode={} {}", lane,
                                             cartographic ? "cartographic" : "world", line));
            }
            t.check(first && other && aligned,
                    "dispatch views: each view retains its own mesh and records");
        }
    }
    std::sort(matrix.begin(), matrix.end());
    std::string all;
    for (const auto& line : matrix) all += line + '\n';
    for (u32 id = 0; id <= static_cast<u32>(MeshTechnique::AeonCZAR); ++id) {
        const std::string name = renderer::mesh_technique_name(static_cast<MeshTechnique>(id));
        for (int lane = 0; lane < 3; ++lane)
            for (const char* view : {"world", "cartographic-main", "cartographic-minimap"}) {
                // Build ghosts are engine-internal and never gathered by the minimap.
                if (id == static_cast<u32>(MeshTechnique::UnitPlace) &&
                    std::string(view) == "cartographic-minimap")
                    continue;
                bool found = false;
                for (const auto& line : matrix) {
                    const bool target =
                        std::string(view) == "world"
                            ? line.find("mode=world") != std::string::npos &&
                                  line.find("view=0") != std::string::npos
                            : line.find(std::string("stage=") + view + " ") != std::string::npos;
                    found |= target && line.find(fmt::format("fidelity={} ", lane)) == 0 &&
                             line.find("technique=" + name + " ") != std::string::npos;
                }
                t.check(found,
                        fmt::format("dispatch coverage: {} fidelity={} view={}", name, lane, view));
            }
    }
    const auto fact = [&](int fidelity, const std::string& text, bool expected = true) {
        bool found = false;
        for (const auto& line : matrix)
            found |= line.find(fmt::format("fidelity={} mode=world ", fidelity)) == 0 &&
                     line.find("view=0") != std::string::npos &&
                     line.find(text) != std::string::npos;
        t.check(found == expected, fmt::format("dispatch fact: fidelity={} {} ({})", fidelity, text,
                                               expected ? "present" : "absent"));
    };
    for (int fidelity = 0; fidelity < 3; ++fidelity) {
        fact(fidelity, "technique=CybranBuild stage=pre-water pass=1 pipeline=overlay");
        fact(fidelity, "technique=AeonBuild stage=pre-water pass=1", fidelity > 0);
        fact(fidelity, "technique=ShieldCybran stage=post-effect pass=1", fidelity > 0);
    }
    fact(1, "drawn=Seraphim lane=0 shader_id=4 shadow=0");
    fact(0, "technique=AeonCZAR stage=pre-water pass=0 pipeline=mesh drawn=Aeon lane=0");
    fact(1, "technique=AeonCZAR stage=pre-water pass=0 pipeline=mesh drawn=Aeon lane=1");
    fact(2, "technique=AeonCZAR stage=pre-water pass=0 pipeline=mesh drawn=AeonCZAR lane=2");
    fact(1, "drawn=PhaseShield lane=0 shader_id=25 shadow=0");
    fact(2, "drawn=SeraphimPersonalShield lane=2 shader_id=26 shadow=2");
    for (const char* name : {"Unit",
                             "Aeon",
                             "Insect",
                             "Metal",
                             "Seraphim",
                             "UEFBuild",
                             "AeonBuild",
                             "CybranBuild",
                             "SeraphimBuild",
                             "NormalMappedAlpha",
                             "NormalMappedGlow",
                             "AlphaFade",
                             "UEFBuildCube",
                             "AeonBuildPuddle",
                             "BlackenedNormalMappedAlpha",
                             "VertexNormal",
                             "NormalMappedTerrain",
                             "UndulatingNormalMappedAlpha",
                             "PhaseShield",
                             "SeraphimPersonalShield",
                             "CommandFeedback",
                             "CommandFeedback2",
                             "RallyPoint",
                             "MapImager",
                             "OuterCloud",
                             "NukeUEF",
                             "NukeEMP",
                             "PhalanxEffect",
                             "UEFQuantumGate",
                             "CommandFeedback3",
                             "CommandFeedback4",
                             "AeonCZAR"}) {
        fact(2, fmt::format("technique={} stage=reflection ", name));
        fact(0, fmt::format("technique={} stage=reflection ", name), false);
        fact(1, fmt::format("technique={} stage=reflection ", name), false);
    }
    for (int fidelity = 0; fidelity < 3; ++fidelity)
        for (const char* stage : {"cartographic-main", "cartographic-minimap"}) {
            bool found = false;
            for (const auto& line : matrix)
                found |= line.find(fmt::format("fidelity={} ", fidelity)) == 0 &&
                         line.find(fmt::format("technique=Unit stage={} pass=1 ", stage)) !=
                             std::string::npos;
            t.check(found, fmt::format("dispatch submitted pass: fidelity={} {}", fidelity, stage));
        }
    for (u32 instance = 0; instance < 2; ++instance) {
        bool found = false;
        const std::string position =
            instance == 0 ? "128.0000 0.5000 128.0000 1.0000" : "128.0000 0.7500 128.0000 1.0000";
        for (const auto& line : matrix)
            found |= line.find("view=0") != std::string::npos &&
                     line.find("technique=UEFBuild stage=pre-water ") != std::string::npos &&
                     line.find(position) != std::string::npos &&
                     line.find(fmt::format("instance={} ", instance)) != std::string::npos;
        t.check(found,
                fmt::format("dispatch instance order: overlapping UEFBuild instance={}", instance));
    }
    const auto role_in = [&](const std::string& role, std::initializer_list<const char*> stages) {
        for (const auto& line : matrix)
            for (const char* stage : stages)
                if (line.find("pipeline=" + role + " ") != std::string::npos &&
                    line.find(std::string("stage=") + stage + " ") != std::string::npos)
                    return true;
        return false;
    };
    for (const char* role :
         {"mesh", "fade", "overlay", "cube", "outer_cloud", "nuke_emp", "nuke_quantum", "phalanx",
          "quantum_gate", "feedback", "glow", "shield:Blend", "shield:BlendUnculled",
          "shield:AddRGB", "shield:AddRGBA", "shield:Fill", "shield:BlendDepthWrite"}) {
        t.check(role_in(role, {"reflection", "pre-water", "post-water", "post-effect"}),
                std::string("dispatch world role coverage: ") + role);
    }
    t.check(role_in("shadow", {"shadow"}), "dispatch role coverage: shadow");
    for (const char* role :
         {"cartographic:UnitColour", "cartographic:UnitGlow", "cartographic:Opaque",
          "cartographic:OpaqueAlpha", "cartographic:Shield"})
        t.check(role_in(role, {"cartographic-main", "cartographic-minimap"}),
                std::string("dispatch cartographic role coverage: ") + role);
    for (const char* stage : {"shadow", "reflection", "pre-water", "post-water", "post-effect",
                              "cartographic-main", "cartographic-minimap"})
        t.check(all.find(std::string("stage=") + stage + " ") != std::string::npos,
                std::string("dispatch stage coverage: ") + stage);
    t.check(all.find("bucket=fading") != std::string::npos &&
                all.find("order=") != std::string::npos,
            "dispatch dump: fading bucket and draw position");
    for (const char* stage : {"pre-water", "shadow", "cartographic-main", "cartographic-minimap"}) {
        bool numeric = false;
        for (const auto& line : matrix)
            numeric |= line.find(std::string("stage=") + stage + " ") != std::string::npos &&
                       line.find("shader_id=") != std::string::npos &&
                       line.find("shader_id=none") == std::string::npos;
        t.check(numeric, std::string("dispatch numeric shader id: ") + stage);
    }
    if (const char* file = std::getenv("OSC_DISPATCH_MATRIX")) {
        std::ofstream out(file);
        out << all;
        t.check(static_cast<bool>(out), "dispatch matrix: written to requested file");
    }
    (void)ui.lua_state.do_string("for i = 0, 2 do local v = rawget(_G, '__osc_matrix_view' .. i) "
                                 "if v then v:Destroy() end end\n");
    ui::WorldView::set_default_mini_lod(ui::WorldView::kDefaultMiniLod);
    lua_pushstring(L, core::kWorldUiActiveKey);
    lua_pushnil(L);
    lua_rawset(L, LUA_REGISTRYINDEX);
    spdlog::info("Dispatch matrix: {}/{} passed", t.pass, t.pass + t.fail);
}
} // namespace osc::test
