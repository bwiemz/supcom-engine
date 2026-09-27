#include <catch2/catch_test_macros.hpp>

#include "renderer/mesh_cache.hpp"

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
