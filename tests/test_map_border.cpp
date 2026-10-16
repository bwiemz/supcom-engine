// The map's border (Moho's MapImager; faf-re render/MapImager.cpp): each
// mesh MapBorderAdd names, unrotated over the heightfield's centre, midway
// between its lowest and highest plus the map's imager elevation offset,
// scaled by its width times the mesh blueprint's UniformScale; drawn by the
// MapImager technique, which casts no shadow.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "map/heightmap.hpp"
#include "renderer/map_border.hpp"
#include "renderer/mesh_cache.hpp"
#include "renderer/shadow_camera.hpp"

extern "C" {
#include <lua.h>
}

#include <cmath>
#include <string>
#include <vector>

using namespace osc;
using namespace osc::renderer;
using Catch::Matchers::WithinAbs;

namespace {

struct LuaGuard {
    lua_State* L = lua_open();
    ~LuaGuard() { lua_close(L); }
};

/// MapBorderAdd's list in the registry, as l_MapBorderAdd keeps it.
void set_border_list(lua_State* L, const std::vector<const char*>& meshes) {
    lua_pushstring(L, "__osc_map_border_meshes");
    lua_newtable(L);
    int i = 0;
    for (const char* mesh : meshes) {
        lua_pushstring(L, mesh);
        lua_rawseti(L, -2, ++i);
    }
    lua_rawset(L, LUA_REGISTRYINDEX);
}

} // namespace

TEST_CASE("A border mesh sits over the field's centre, scaled by its width", "[map_border]") {
    // A 512 x 256 field from 12 below to 40 above, offset 3 up; the retail
    // border's UniformScale (1/512) makes it the field's width over 512
    const Box field{{0.0f, -12.0f, 0.0f}, {512.0f, 40.0f, 256.0f}};
    const auto m = map_border_model(field, 3.0f, 1.0f / 512.0f);
    CHECK_THAT(m[0], WithinAbs(1.0, 1e-6));
    CHECK_THAT(m[5], WithinAbs(1.0, 1e-6));
    CHECK_THAT(m[10], WithinAbs(1.0, 1e-6));
    CHECK_THAT(m[12], WithinAbs(256.0, 1e-4));
    CHECK_THAT(m[13], WithinAbs(14.0 + 3.0, 1e-4)); // (-12 + 40) / 2, plus the offset
    CHECK_THAT(m[14], WithinAbs(128.0, 1e-4));
    CHECK(m[15] == 1.0f);
    // Unrotated
    for (const int i : {1, 2, 3, 4, 6, 7, 8, 9, 11}) CHECK(m[i] == 0.0f);

    // A larger map's border is larger by its width alone
    const Box wide{{0.0f, 0.0f, 0.0f}, {1024.0f, 10.0f, 512.0f}};
    CHECK_THAT(map_border_model(wide, 0.0f, 1.0f / 512.0f)[0], WithinAbs(2.0, 1e-6));
}

TEST_CASE("HeightBounds' box is the whole field's, and follows its changes", "[map_border]") {
    // 256 x 256, flat at 10 but for a 50-high corner cell
    std::vector<u16> raw(257 * 257, 10 * 128);
    raw[0] = 50 * 128;
    map::Heightmap h(256, 256, 1.0f / 128.0f, raw);
    HeightBounds bounds(h);
    Box box = bounds.box();
    CHECK(box.min[0] == 0.0f);
    CHECK(box.min[2] == 0.0f);
    CHECK(box.max[0] == 256.0f);
    CHECK(box.max[2] == 256.0f);
    CHECK_THAT(box.min[1], WithinAbs(10.0, 1e-4));
    CHECK_THAT(box.max[1], WithinAbs(50.0, 1e-4));

    // The corner flattened (as a structure's site is): the top comes down
    raw[0] = 10 * 128;
    const map::Heightmap flat(256, 256, 1.0f / 128.0f, raw);
    bounds.update(flat, 0, 0, 2, 2);
    box = bounds.box();
    CHECK_THAT(box.max[1], WithinAbs(10.0, 1e-4));
}

TEST_CASE("The border's meshes are MapBorderAdd's list, in order", "[map_border]") {
    CHECK(map_border_meshes(nullptr).empty());
    LuaGuard lua;
    lua_State* L = lua.L;
    CHECK(map_border_meshes(L).empty()); // none added
    CHECK(lua_gettop(L) == 0);

    set_border_list(
        L, {"/meshes/game/map-border_squ_aeon_mesh", "/meshes/game/map-border_squ_aeon_a_mesh"});
    const std::vector<std::string> meshes = map_border_meshes(L);
    REQUIRE(meshes.size() == 2);
    CHECK(meshes[0] == "/meshes/game/map-border_squ_aeon_mesh");
    CHECK(meshes[1] == "/meshes/game/map-border_squ_aeon_a_mesh");
    CHECK(lua_gettop(L) == 0);

    // MapBorderClear's empty list
    set_border_list(L, {});
    CHECK(map_border_meshes(L).empty());
    CHECK(lua_gettop(L) == 0);
}

TEST_CASE("MapImager draws before the water, unblended, and casts no shadow", "[map_border]") {
    CHECK(mesh_technique("MapImager") == MeshTechnique::MapImager);
    CHECK_FALSE(has_depth_stage(MeshTechnique::MapImager));
    CHECK_FALSE(is_post_water_technique(MeshTechnique::MapImager));
    CHECK_FALSE(is_post_effect_technique(MeshTechnique::MapImager));
    CHECK_FALSE(is_blended_technique(MeshTechnique::MapImager));
}
