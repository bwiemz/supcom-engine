#include <catch2/catch_test_macros.hpp>

#include "ui/user_decals.hpp"

#include <limits>
#include <string>

using osc::ui::UserDecals;

TEST_CASE("A UserDecal lies at its position less half its scale", "[ui][decal]") {
    UserDecals decals;
    const auto id = decals.create();
    decals.set_scale(id, 60, 1, 40);
    decals.set_position(id, 100, 20, 200);
    const UserDecals::Decal* d = decals.get(id);
    REQUIRE(d);
    CHECK(d->corner == std::array<float, 3>{70, 20, 180});
}

TEST_CASE("A UserDecal's SetScale places it again from its corner, as Moho's does", "[ui][decal]") {
    UserDecals decals;
    const auto id = decals.create();
    decals.set_position(id, 100, 20, 200);
    decals.set_scale(id, 10, 1, 10);
    CHECK(decals.get(id)->corner == std::array<float, 3>{94.5f, 20, 194.5f});
}

TEST_CASE("A UserDecal keeps its place through a NaN position", "[ui][decal]") {
    UserDecals decals;
    const auto id = decals.create();
    decals.set_position(id, 4, 5, 6);
    decals.set_position(id, std::numeric_limits<float>::quiet_NaN(), 0, 0);
    CHECK(decals.get(id)->corner == std::array<float, 3>{3.5f, 5, 5.5f});
}

TEST_CASE("UserDecals draw once textured, in the order they got their textures", "[ui][decal]") {
    UserDecals decals;
    const auto a = decals.create();
    const auto b = decals.create();
    const auto c = decals.create();
    decals.set_texture(b, "/b.dds");
    decals.set_texture(a, "/a.dds");
    auto splats = decals.splats();
    REQUIRE(splats.size() == 2);
    CHECK(splats[0]->texture == "/b.dds");
    CHECK(splats[1]->texture == "/a.dds");
    decals.set_texture(b, "/b2.dds");
    decals.destroy(a);
    splats = decals.splats();
    REQUIRE(splats.size() == 1);
    CHECK(splats[0]->texture == "/b2.dds");
    CHECK(decals.get(c) != nullptr);
    CHECK(decals.get(a) == nullptr);
}
