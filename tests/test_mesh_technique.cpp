#include <catch2/catch_test_macros.hpp>

#include "renderer/mesh_cache.hpp"
#include "renderer/cartographic_technique.hpp"
#include "renderer/unit_renderer.hpp"

#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace osc::renderer;

TEST_CASE("mesh_technique: a LOD's ShaderName picks FA's technique", "[renderer][mesh]") {
    CHECK(mesh_technique("Unit") == MeshTechnique::Unit);
    CHECK(mesh_technique("Aeon") == MeshTechnique::Aeon);
    CHECK(mesh_technique("Insect") == MeshTechnique::Insect);
    CHECK(mesh_technique("Metal") == MeshTechnique::Metal);
    CHECK(mesh_technique("Seraphim") == MeshTechnique::Seraphim);
    // The build meshes retail's ExtractBuildMeshBlueprint makes (M211f).
    CHECK(mesh_technique("UEFBuild") == MeshTechnique::UEFBuild);
    CHECK(mesh_technique("AeonBuild") == MeshTechnique::AeonBuild);
    CHECK(mesh_technique("CybranBuild") == MeshTechnique::CybranBuild);
    CHECK(mesh_technique("SeraphimBuild") == MeshTechnique::SeraphimBuild);
    // The build effects' and props' (M211g).
    CHECK(mesh_technique("NormalMappedAlpha") == MeshTechnique::NormalMappedAlpha);
    CHECK(mesh_technique("NormalMappedGlow") == MeshTechnique::NormalMappedGlow);
    CHECK(mesh_technique("AlphaFade") == MeshTechnique::AlphaFade);
    CHECK(mesh_technique("UEFBuildCube") == MeshTechnique::UEFBuildCube);
    CHECK(mesh_technique("AeonBuildPuddle") == MeshTechnique::AeonBuildPuddle);
    CHECK(mesh_technique("BlackenedNormalMappedAlpha") ==
          MeshTechnique::BlackenedNormalMappedAlpha);
    // The props' (M211i).
    CHECK(mesh_technique("VertexNormal") == MeshTechnique::VertexNormal);
    CHECK(mesh_technique("TMeshNoNormals") == MeshTechnique::VertexNormal);
    CHECK(mesh_technique("NormalMappedTerrain") == MeshTechnique::NormalMappedTerrain);
    CHECK(mesh_technique("UndulatingNormalMappedAlpha") ==
          MeshTechnique::UndulatingNormalMappedAlpha);
    // The explosions' and effects', by their legacy names too.
    CHECK(mesh_technique("Effect") == MeshTechnique::Effect);
    CHECK(mesh_technique("TMeshAdd") == MeshTechnique::Effect);
    CHECK(mesh_technique("Cloud") == MeshTechnique::Cloud);
    CHECK(mesh_technique("TMeshCloud") == MeshTechnique::Cloud);
    CHECK(mesh_technique("OuterCloud") == MeshTechnique::OuterCloud);
    CHECK(mesh_technique("TMeshOuterCloud") == MeshTechnique::OuterCloud);
    CHECK(mesh_technique("NukeUEF") == MeshTechnique::NukeUEF);
    // Anything unported draws as Unit.
    CHECK(mesh_technique("") == MeshTechnique::Unit);
    CHECK(mesh_technique("Clutter") == MeshTechnique::Unit);
    CHECK(mesh_technique("uefbuild") == MeshTechnique::Unit);
}

TEST_CASE("resolve_shader_name: Moho's legacy ShaderNames", "[renderer][mesh]") {
    // ShaderDictionary's remaps (ResolveShaderAnnotationName).
    CHECK(resolve_shader_name("TMeshNoLighting") == "Flat");
    CHECK(resolve_shader_name("TMeshNoNormals") == "VertexNormal");
    CHECK(resolve_shader_name("TMeshAlpha") == "NormalMappedAlpha");
    CHECK(resolve_shader_name("TMeshGlow") == "NormalMappedGlow");
    CHECK(resolve_shader_name("TMeshTerrain") == "NormalMappedTerrain");
    CHECK(resolve_shader_name("Simple") == "Unit");
    CHECK(resolve_shader_name("Team") == "Unit");
    CHECK(resolve_shader_name("TMeshAlphaGlowFade") == "UnitBuild");
    CHECK(resolve_shader_name("TMeshMetalBuild") == "AeonBuild");
    CHECK(resolve_shader_name("TMeshShield") == "Shield");
    CHECK(resolve_shader_name("TMeshZFill") == "ShieldFill");
    CHECK(resolve_shader_name("TMeshAdd") == "Effect");
    CHECK(resolve_shader_name("TMeshExplosion") == "Explosion");
    CHECK(resolve_shader_name("TMeshCloud") == "Cloud");
    CHECK(resolve_shader_name("TMeshOuterCloud") == "OuterCloud");
    CHECK(resolve_shader_name("TMeshEMPNuke") == "NukeEMP");
    CHECK(resolve_shader_name("TMeshQuantumNuke") == "NukeQuantum");
    CHECK(resolve_shader_name("TMeshTemporalBubble") == "TemporalBubble");
    // Others pass through; an empty one is Unit.
    CHECK(resolve_shader_name("Aeon") == "Aeon");
    CHECK(resolve_shader_name("tmeshglow") == "tmeshglow");
    CHECK(resolve_shader_name("") == "Unit");
    // mesh_technique resolves first.
    CHECK(mesh_technique("TMeshGlow") == MeshTechnique::NormalMappedGlow);
    CHECK(mesh_technique("TMeshAlpha") == MeshTechnique::NormalMappedAlpha);
    CHECK(mesh_technique("TMeshMetalBuild") == MeshTechnique::AeonBuild);
    CHECK(mesh_technique("Team") == MeshTechnique::Unit);
}

TEST_CASE("is_build_technique: the four build techniques alone", "[renderer][mesh]") {
    CHECK(is_build_technique(MeshTechnique::UEFBuild));
    CHECK(is_build_technique(MeshTechnique::AeonBuild));
    CHECK(is_build_technique(MeshTechnique::CybranBuild));
    CHECK(is_build_technique(MeshTechnique::SeraphimBuild));
    CHECK_FALSE(is_build_technique(MeshTechnique::Unit));
    CHECK_FALSE(is_build_technique(MeshTechnique::Aeon));
    CHECK_FALSE(is_build_technique(MeshTechnique::Insect));
    CHECK_FALSE(is_build_technique(MeshTechnique::Metal));
    CHECK_FALSE(is_build_technique(MeshTechnique::Seraphim));
    CHECK_FALSE(is_build_technique(MeshTechnique::UEFBuildCube));
}

TEST_CASE("The explosions' techniques: mesh.fx's stages and blending", "[renderer][mesh]") {
    // STAGE_POSTWATER + STAGE_POSTEFFECT
    CHECK(is_post_effect_technique(MeshTechnique::Effect));
    CHECK(is_post_effect_technique(MeshTechnique::Cloud));
    // STAGE_POSTWATER + STAGE_PREEFFECT
    CHECK(is_post_water_technique(MeshTechnique::OuterCloud));
    CHECK(is_post_water_technique(MeshTechnique::NukeUEF));
    for (const MeshTechnique t : {MeshTechnique::Effect, MeshTechnique::Cloud,
                                  MeshTechnique::OuterCloud, MeshTechnique::NukeUEF}) {
        CHECK(is_post_effect_technique(t) != is_post_water_technique(t));
        CHECK_FALSE(has_depth_stage(t)); // no STAGE_DEPTH: no shadow
    }
    // Effect sets no alpha state: opaque. The rest blend by SrcAlpha.
    CHECK_FALSE(is_blended_technique(MeshTechnique::Effect));
    CHECK(is_blended_technique(MeshTechnique::Cloud));
    CHECK(is_blended_technique(MeshTechnique::OuterCloud));
    CHECK(is_blended_technique(MeshTechnique::NukeUEF));
}

TEST_CASE("is_blended_technique: what draws after the opaque meshes", "[renderer][mesh]") {
    CHECK(is_blended_technique(MeshTechnique::UEFBuild));
    CHECK(is_blended_technique(MeshTechnique::SeraphimBuild));
    CHECK(is_blended_technique(MeshTechnique::AlphaFade));
    CHECK(is_blended_technique(MeshTechnique::UEFBuildCube));
    CHECK(is_blended_technique(MeshTechnique::VertexNormal));
    CHECK_FALSE(is_blended_technique(MeshTechnique::Unit));
    CHECK_FALSE(is_blended_technique(MeshTechnique::NormalMappedTerrain));
    CHECK_FALSE(is_blended_technique(MeshTechnique::UndulatingNormalMappedAlpha));
    CHECK_FALSE(is_blended_technique(MeshTechnique::NormalMappedAlpha));
    CHECK_FALSE(is_blended_technique(MeshTechnique::NormalMappedGlow));
    CHECK_FALSE(is_blended_technique(MeshTechnique::AeonBuildPuddle));
    CHECK_FALSE(is_blended_technique(MeshTechnique::BlackenedNormalMappedAlpha));
}

TEST_CASE("The shields' techniques: their names, stage and parameter (M211k)", "[renderer][mesh]") {
    // The shield, fill and impact meshes' ShaderNames (effects.scd)
    CHECK(mesh_technique("ShieldUEF") == MeshTechnique::ShieldUEF);
    CHECK(mesh_technique("ShieldCybran") == MeshTechnique::ShieldCybran);
    CHECK(mesh_technique("ShieldAeon") == MeshTechnique::ShieldAeon);
    CHECK(mesh_technique("ShieldSeraphim") == MeshTechnique::ShieldSeraphim);
    CHECK(mesh_technique("ShieldFill") == MeshTechnique::ShieldFill);
    CHECK(mesh_technique("TMeshZFill") == MeshTechnique::ShieldFill); // the legacy name
    CHECK(mesh_technique("ShieldImpact") == MeshTechnique::ShieldImpact);
    CHECK(mesh_technique("CybranShieldImpact") == MeshTechnique::CybranShieldImpact);
    // The personal shields' (M211l) aren't bubbles
    CHECK_FALSE(is_shield_technique(MeshTechnique::PhaseShield));
    CHECK_FALSE(is_shield_technique(MeshTechnique::SeraphimPersonalShield));

    // All seven draw after the effects (POSTWATER + POSTEFFECT), and only
    // they: not among the post-water meshes drawn before the effects
    for (const MeshTechnique t :
         {MeshTechnique::ShieldUEF, MeshTechnique::ShieldCybran, MeshTechnique::ShieldAeon,
          MeshTechnique::ShieldSeraphim, MeshTechnique::ShieldFill, MeshTechnique::ShieldImpact,
          MeshTechnique::CybranShieldImpact}) {
        CHECK(is_shield_technique(t));
        CHECK(is_post_effect_technique(t));
        CHECK_FALSE(has_depth_stage(t)); // no shadow
        CHECK_FALSE(is_post_water_technique(t));
        CHECK_FALSE(is_blended_technique(t));
    }
    for (const MeshTechnique t :
         {MeshTechnique::Unit, MeshTechnique::Seraphim, MeshTechnique::UEFBuild,
          MeshTechnique::AlphaFade, MeshTechnique::VertexNormal,
          MeshTechnique::UndulatingNormalMappedAlpha}) {
        CHECK_FALSE(is_shield_technique(t));
        CHECK_FALSE(is_post_effect_technique(t));
    }
    // What else casts no shadow: a unit Aeon are building, UEF's build slices
    CHECK_FALSE(has_depth_stage(MeshTechnique::AeonBuild));
    CHECK_FALSE(has_depth_stage(MeshTechnique::AlphaFade));
    CHECK(has_depth_stage(MeshTechnique::Unit));
    CHECK(has_depth_stage(MeshTechnique::UEFBuild));

    // PARAM_FRACTIONHEALTH for the shields and the fill; the impacts' is
    // unused, the rest take the fraction complete
    CHECK(mesh_parameter(MeshTechnique::ShieldUEF) == MeshParameter::FractionHealth);
    CHECK(mesh_parameter(MeshTechnique::ShieldCybran) == MeshParameter::FractionHealth);
    CHECK(mesh_parameter(MeshTechnique::ShieldAeon) == MeshParameter::FractionHealth);
    CHECK(mesh_parameter(MeshTechnique::ShieldSeraphim) == MeshParameter::FractionHealth);
    CHECK(mesh_parameter(MeshTechnique::ShieldFill) == MeshParameter::FractionHealth);
    CHECK(mesh_parameter(MeshTechnique::ShieldImpact) == MeshParameter::FractionComplete);
    CHECK(mesh_parameter(MeshTechnique::CybranShieldImpact) == MeshParameter::FractionComplete);
    CHECK(mesh_parameter(MeshTechnique::UEFBuild) == MeshParameter::FractionComplete);
    CHECK(mesh_parameter(MeshTechnique::Unit) == MeshParameter::FractionComplete);
}

TEST_CASE("The shields' passes: mesh.fx's states (M211k)", "[renderer][mesh]") {
    const auto is = [](MeshTechnique t, ShieldState state, osc::u32 count) {
        const ShieldPasses p = shield_passes(t);
        return p.state == state && p.count == count;
    };
    // Rasterizer_Cull_None: UEF's shield and Cybran's impact
    CHECK(is(MeshTechnique::ShieldUEF, ShieldState::BlendUnculled, 1));
    CHECK(is(MeshTechnique::CybranShieldImpact, ShieldState::BlendUnculled, 1));
    // Cybran's P0 and P1 (ShieldPositionNormalOffsetVS), both blended
    CHECK(is(MeshTechnique::ShieldCybran, ShieldState::Blend, 2));
    CHECK(is(MeshTechnique::ShieldAeon, ShieldState::Blend, 1));
    // AlphaBlend_SrcAlpha_One_Write_RGB, and _RGBA for the impact
    CHECK(is(MeshTechnique::ShieldSeraphim, ShieldState::AddRGB, 1));
    CHECK(is(MeshTechnique::ShieldImpact, ShieldState::AddRGBA, 1));
    // AlphaBlend_Disable_Write_None with the depth written
    CHECK(is(MeshTechnique::ShieldFill, ShieldState::Fill, 1));
}

TEST_CASE("The personal shields' techniques: the unit, then its shell (M211l)",
          "[renderer][mesh]") {
    CHECK(mesh_technique("PhaseShield") == MeshTechnique::PhaseShield);
    CHECK(mesh_technique("SeraphimPersonalShield") == MeshTechnique::SeraphimPersonalShield);
    CHECK(is_personal_shield_technique(MeshTechnique::PhaseShield));
    CHECK(is_personal_shield_technique(MeshTechnique::SeraphimPersonalShield));
    CHECK_FALSE(is_personal_shield_technique(MeshTechnique::Unit));
    CHECK_FALSE(is_personal_shield_technique(MeshTechnique::ShieldUEF));
    // P0 is the unit: NormalMappedPS, or the Seraphim's UnitFalloffPS
    CHECK(base_technique(MeshTechnique::PhaseShield) == MeshTechnique::Unit);
    CHECK(base_technique(MeshTechnique::SeraphimPersonalShield) == MeshTechnique::Seraphim);
    CHECK(base_technique(MeshTechnique::Aeon) == MeshTechnique::Aeon);
    CHECK(base_technique(MeshTechnique::ShieldCybran) == MeshTechnique::ShieldCybran);
    // A unit's stages: before the water and the effects, casting a shadow
    for (const MeshTechnique t :
         {MeshTechnique::PhaseShield, MeshTechnique::SeraphimPersonalShield}) {
        CHECK_FALSE(is_post_water_technique(t));
        CHECK_FALSE(is_post_effect_technique(t));
        CHECK(has_depth_stage(t));
        CHECK_FALSE(is_blended_technique(t));
    }
}

TEST_CASE("UnitPlace: a build ghost's stage, after the effects and casting no shadow",
          "[renderer][mesh]") {
    CHECK(is_post_effect_technique(MeshTechnique::UnitPlace));
    CHECK_FALSE(is_post_water_technique(MeshTechnique::UnitPlace));
    CHECK_FALSE(has_depth_stage(MeshTechnique::UnitPlace));
    CHECK_FALSE(is_blended_technique(MeshTechnique::UnitPlace));
    CHECK(base_technique(MeshTechnique::UnitPlace) == MeshTechnique::UnitPlace);
}

TEST_CASE("drawn_technique: a ghost group draws as UnitPlace, whatever its mesh's",
          "[renderer][mesh]") {
    GPUMesh mesh;
    mesh.technique = MeshTechnique::Aeon;
    MeshDrawGroup group;
    group.mesh = &mesh;
    CHECK(drawn_technique(group) == MeshTechnique::Aeon);
    group.ghost = true;
    CHECK(drawn_technique(group) == MeshTechnique::UnitPlace);
}

namespace {
struct LogCapture {
    std::ostringstream text;
    std::shared_ptr<spdlog::logger> previous = spdlog::default_logger();
    LogCapture() {
        spdlog::set_default_logger(std::make_shared<spdlog::logger>(
            "capture", std::make_shared<spdlog::sinks::ostream_sink_st>(text)));
    }
    ~LogCapture() { spdlog::set_default_logger(previous); }
    size_t count(const std::string& line) const {
        const std::string all = text.str();
        size_t n = 0;
        for (size_t at = all.find(line); at != std::string::npos; at = all.find(line, at + 1)) ++n;
        return n;
    }
};
} // namespace

TEST_CASE("mesh_technique: a ShaderName with no technique warns once and draws as Unit",
          "[renderer][mesh]") {
    LogCapture log;
    for (int i = 0; i < 3; ++i) {
        CHECK(mesh_technique("NoSuchShaderA") == MeshTechnique::Unit);
        CHECK(mesh_technique("NoSuchShaderB") == MeshTechnique::Unit);
    }
    CHECK(log.count("no technique for ShaderName 'NoSuchShaderA'") == 1);
    CHECK(log.count("no technique for ShaderName 'NoSuchShaderB'") == 1);

    for (const char* quiet : {"TMeshAlpha", "Aeon", "", "Unit", "Simple", "Wreckage"})
        mesh_technique(quiet);
    CHECK(log.count("no technique") == 2);
}

namespace {
std::vector<std::string> names_listed(const std::string& file) {
    std::ifstream in(std::string(OSC_SOURCE_DIR) + "/tools/" + file);
    std::vector<std::string> names;
    for (std::string line; std::getline(in, line);) {
        line = line.substr(0, line.find('#'));
        line.erase(line.find_last_not_of(" \t\r") + 1);
        if (!line.empty()) names.push_back(line);
    }
    return names;
}
} // namespace

TEST_CASE("mesh_technique: every retail ShaderName has a technique, or is listed as having none",
          "[renderer][mesh]") {
    // What mesh_technique returns, not what its source reads as: tools/check_shader_names.py
    // reads the source's text, so a name mapped to the wrong technique passes it.
    const std::vector<std::string> retail = names_listed("shader_names.txt");
    const std::vector<std::string> listed = names_listed("shader_names_baseline.txt");
    REQUIRE(retail.size() > 20);
    const std::set<std::string> none_yet(listed.begin(), listed.end());
    for (const std::string& name : retail) {
        const std::string resolved = resolve_shader_name(name);
        if (resolved == "Unit" || resolved == "Wreckage" || none_yet.count(resolved)) continue;
        INFO(name << " resolves to " << resolved);
        CHECK(mesh_technique(resolved) != MeshTechnique::Unit);
    }
}

TEST_CASE("mesh technique dump names round trip", "[renderer][mesh]") {
    // Build ghosts select UnitPlace internally; ShaderName still falls back to Unit.
    constexpr std::array engine_internal = {MeshTechnique::UnitPlace};
    for (const auto technique : engine_internal)
        CHECK(mesh_technique(mesh_technique_name(technique)) == MeshTechnique::Unit);
    for (osc::u32 id = 0; id <= static_cast<osc::u32>(MeshTechnique::NukeUEF); ++id) {
        const auto technique = static_cast<MeshTechnique>(id);
        if (std::find(engine_internal.begin(), engine_internal.end(), technique) !=
            engine_internal.end())
            continue;
        INFO(mesh_technique_name(technique));
        CHECK(mesh_technique(mesh_technique_name(technique)) == technique);
    }
}

TEST_CASE("cartographic dispatch reports the selected pipeline handle", "[renderer][mesh]") {
    const std::array<VkPipeline, kCartographicMeshStates> pipelines = {
        reinterpret_cast<VkPipeline>(1), reinterpret_cast<VkPipeline>(2),
        reinterpret_cast<VkPipeline>(3), reinterpret_cast<VkPipeline>(4),
        reinterpret_cast<VkPipeline>(5)};
    CHECK(std::string(cartographic_pipeline_role(pipelines[0], pipelines)) ==
          "cartographic:UnitColour");
    CHECK(std::string(cartographic_pipeline_role(pipelines[4], pipelines)) ==
          "cartographic:Shield");
}
