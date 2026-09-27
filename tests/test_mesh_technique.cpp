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
    // Anything unported draws as Unit.
    CHECK(mesh_technique("") == MeshTechnique::Unit);
    CHECK(mesh_technique("UEFBuildCube") == MeshTechnique::Unit);
    CHECK(mesh_technique("uefbuild") == MeshTechnique::Unit);
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
}
