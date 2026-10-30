#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "renderer/camera.hpp"
#include "renderer/overlay_renderer.hpp"
#include "renderer/renderer.hpp"
#include "renderer/texture_cache.hpp"
#include "sim/world_snapshot.hpp"

#include <cmath>
#include <string>
#include <utility>
#include <vector>

using osc::f32;
using osc::renderer::convex_rows;
using osc::sim::EntityId;

TEST_CASE("A footprint on screen is filled row by row, within its edges", "[overlay]") {
    const auto rect = convex_rows({10, 30, 30, 10}, {0, 0, 8, 8}, 2.0f);
    REQUIRE(rect.size() == 4);
    for (const auto& q : rect) {
        CHECK(q[0] == Catch::Approx(10.0f));
        CHECK(q[2] == Catch::Approx(20.0f));
        CHECK(q[3] == Catch::Approx(2.0f));
    }

    const auto diamond = convex_rows({0, 10, 0, -10}, {-10, 0, 10, 0}, 2.0f);
    REQUIRE(diamond.size() == 10);
    f32 area = 0.0f;
    for (const auto& q : diamond) {
        const f32 mid = q[1] + q[3] * 0.5f;
        CHECK(q[2] == Catch::Approx(2.0f * (10.0f - std::abs(mid))));
        CHECK(q[0] == Catch::Approx(-q[2] * 0.5f));
        area += q[2] * q[3];
    }
    CHECK(area == Catch::Approx(200.0f));

    CHECK(convex_rows({0, 10, 20, 30}, {5, 5, 5, 5}, 2.0f).empty());
}

TEST_CASE("Placing a structure outlines its pad and the standing pads, lit where adjacent",
          "[overlay]") {
    using osc::renderer::BuildGhost;
    using osc::renderer::StructurePad;
    const std::vector<StructurePad> pads = {
        {{26.5f, 36.5f, 34.5f, 44.5f}, 30.5f, 4.0f, 40.5f, EntityId::of(1, 1)},
        {{60.0f, 60.0f, 68.0f, 68.0f}, 64.0f, 2.0f, 64.0f, EntityId::of(2, 1)}};
    const auto any = [](EntityId, const std::string&) { return true; };
    BuildGhost ghost;
    ghost.blueprint_id = "ueb0101";
    ghost.y = 5.0f;
    ghost.pad_x0 = 34.5f;
    ghost.pad_z0 = 39.5f;
    ghost.pad_x1 = 36.5f;
    ghost.pad_z1 = 41.5f;
    // Beside the first pad: both lit; the other dim
    auto out = osc::renderer::build_outlines(pads, ghost, any);
    REQUIRE(out.size() == 3);
    CHECK(out[0].site.x0 == Catch::Approx(34.5f));
    CHECK(out[0].y == Catch::Approx(5.0f));
    CHECK(out[0].color == osc::renderer::kOutlineLit);
    CHECK(out[1].color == osc::renderer::kOutlineLit);
    CHECK(out[1].y == Catch::Approx(4.0f));
    CHECK(out[2].color == osc::renderer::kOutlineDim);
    // A site that can't be built lights nothing and is red
    ghost.valid = false;
    out = osc::renderer::build_outlines(pads, ghost, any);
    CHECK(out[0].color == osc::renderer::kOutlineInvalid);
    CHECK(out[1].color == osc::renderer::kOutlineDim);
    // A drag's line: each site its own outline, a pad lit by one of them
    ghost.valid = true;
    ghost.pad_x0 = 0;
    ghost.pad_z0 = 0;
    ghost.pad_x1 = 2;
    ghost.pad_z1 = 2;
    BuildGhost beside = ghost;
    beside.pad_x0 = 68.0f;
    beside.pad_z0 = 62.0f;
    beside.pad_x1 = 70.0f;
    beside.pad_z1 = 64.0f;
    ghost.line.push_back(beside);
    out = osc::renderer::build_outlines(pads, ghost, any);
    REQUIRE(out.size() == 4);
    CHECK(out[0].color == osc::renderer::kOutlineDim);
    CHECK(out[1].color == osc::renderer::kOutlineLit);
    CHECK(out[2].color == osc::renderer::kOutlineDim);
    CHECK(out[3].color == osc::renderer::kOutlineLit);
}

TEST_CASE("A placed structure is lit by the first structure beside it that gives it a bonus, as "
          "Moho's",
          "[overlay]") {
    // DrawBuildDragAdjacencyHighlights (0x00854B70): each structure tries the
    // previews still dim, and gamemain.OnDetectAdjacencyBonus decides; a
    // preview once lit is passed over (cmp of its colour, 0x00854DA0)
    using osc::renderer::BuildGhost;
    using osc::renderer::StructurePad;
    const std::vector<StructurePad> pads = {
        {{26.5f, 36.5f, 34.5f, 44.5f}, 30.5f, 4.0f, 40.5f, EntityId::of(1, 1)},
        {{36.5f, 36.5f, 44.5f, 44.5f}, 40.5f, 4.0f, 40.5f, EntityId::of(2, 1)}};
    BuildGhost ghost;
    ghost.blueprint_id = "ueb1101";
    ghost.pad_x0 = 34.5f;
    ghost.pad_z0 = 39.5f;
    ghost.pad_x1 = 36.5f;
    ghost.pad_z1 = 41.5f;
    std::vector<std::pair<EntityId, std::string>> asked;
    bool from_first = true;
    const auto bonus = [&](EntityId unit, const std::string& bp) {
        asked.emplace_back(unit, bp);
        return unit != EntityId::of(1, 1) || from_first;
    };
    // Both beside it: the first lights it, the second finds it lit
    auto out = osc::renderer::build_outlines(pads, ghost, bonus);
    REQUIRE(out.size() == 3);
    CHECK(out[0].color == osc::renderer::kOutlineLit);
    CHECK(out[1].color == osc::renderer::kOutlineLit);
    CHECK(out[2].color == osc::renderer::kOutlineDim);
    CHECK(asked == std::vector<std::pair<EntityId, std::string>>{{EntityId::of(1, 1), "ueb1101"}});
    // The first gives no bonus: the second lights it
    asked.clear();
    from_first = false;
    out = osc::renderer::build_outlines(pads, ghost, bonus);
    CHECK(out[0].color == osc::renderer::kOutlineLit);
    CHECK(out[1].color == osc::renderer::kOutlineDim);
    CHECK(out[2].color == osc::renderer::kOutlineLit);
    CHECK(asked.size() == 2);
    // With nothing to ask, nothing lights
    out = osc::renderer::build_outlines(pads, ghost, {});
    CHECK(out[0].color == osc::renderer::kOutlineDim);
    CHECK(out[1].color == osc::renderer::kOutlineDim);
}

TEST_CASE("Placing a structure outlines the focus army's live structures", "[overlay]") {
    osc::sim::WorldSnapshot world;
    osc::sim::EntityRecord factory;
    factory.id = EntityId::of(1, 1);
    factory.army = 0;
    factory.is_unit = true;
    factory.is_structure = true;
    factory.position = {30.5f, 4.0f, 40.5f};
    factory.footprint_size_x = factory.footprint_size_z = 5;
    factory.skirt_size_x = factory.skirt_size_z = 8;
    factory.skirt_offset_x = factory.skirt_offset_z = -1.5f;
    world.entities.push_back(factory);
    osc::sim::EntityRecord tank;
    tank.id = EntityId::of(2, 1);
    tank.army = 0;
    tank.is_unit = true;
    tank.position = {10, 0, 10};
    world.entities.push_back(tank);
    osc::sim::EntityRecord theirs = factory;
    theirs.id = EntityId::of(3, 1);
    theirs.army = 1;
    world.entities.push_back(theirs);
    osc::sim::EntityRecord dying = factory;
    dying.id = EntityId::of(4, 1);
    dying.is_dying = true;
    world.entities.push_back(dying);

    const osc::sim::FrameView view(&world, &world, 1.0f);
    const auto pads = osc::renderer::structure_pads(view, 0);
    REQUIRE(pads.size() == 1);
    CHECK(pads[0].id == EntityId::of(1, 1));
    CHECK(pads[0].site.x0 == Catch::Approx(26.5f));
    CHECK(pads[0].site.x1 == Catch::Approx(34.5f));
    CHECK(pads[0].site.z0 == Catch::Approx(36.5f));
    CHECK(pads[0].y == Catch::Approx(4.0f));
    CHECK(osc::renderer::structure_pads(view, -1).empty()); // an observer's: none
}

TEST_CASE("A ghost touches the pads beside it edge on, its span inside", "[overlay]") {
    using osc::sim::StructureSite;
    const StructureSite factory{26.5f, 36.5f, 34.5f, 44.5f};
    const StructureSite beside{34.5f, 39.5f, 36.5f, 41.5f};
    CHECK(factory.touches(beside));
    CHECK_FALSE(factory.touches({34.5f, 43.5f, 36.5f, 45.5f}));
    CHECK_FALSE(factory.touches({35.5f, 39.5f, 37.5f, 41.5f}));
    CHECK(factory.touches({34.5f, 30.0f, 42.5f, 50.0f}));
    const StructureSite other{36.5f, 36.5f, 44.5f, 44.5f};
    CHECK(other.touches(beside));
}

TEST_CASE("A silo's stored missiles draw nothing over it", "[overlay]") {
    osc::sim::WorldSnapshot world;
    osc::sim::EntityRecord silo;
    silo.id = EntityId::of(1, 1);
    silo.is_unit = true;
    silo.is_structure = true;
    silo.army = 0;
    silo.health = silo.max_health = 100.0f;
    silo.position = {256, 0, 256};
    world.entities.push_back(silo);
    osc::renderer::Camera camera;
    camera.init(512.0f, 512.0f);
    camera.set_input_enabled(false);
    camera.set_viewport(800.0f, 600.0f);
    camera.set_zoom(40.0f);
    camera.set_pitch(0.87f);
    camera.set_target(256.0f, 256.0f);
    const auto vp = camera.view_proj(800.0f / 600.0f);
    osc::renderer::TextureCache textures;
    const auto quads = [&](osc::i32 nukes, osc::i32 tactical) {
        world.entities[0].nuke_silo_ammo = nukes;
        world.entities[0].tactical_silo_ammo = tactical;
        osc::renderer::OverlayRenderer overlays;
        osc::sim::WorldEvents events;
        overlays.update(osc::sim::FrameView(&world, &world, 1.0f), events, camera, vp, nullptr,
                        textures, 800, 600);
        return overlays.quads().size();
    };
    const size_t bars = quads(0, 0);
    CHECK(bars > 0);
    CHECK(quads(2, 0) == bars);
    CHECK(quads(0, 3) == bars);
}

TEST_CASE("A unit's bars aren't drawn while its view spins or is free", "[overlay]") {
    // CUIWorldView::Render draws its icon pass, the unit bars in it, only
    // while its camera neither spins nor is free (cam_Free)
    osc::sim::WorldSnapshot world;
    osc::sim::EntityRecord tank;
    tank.id = EntityId::of(1, 1);
    tank.is_unit = true;
    tank.army = 0;
    tank.health = 50.0f;
    tank.max_health = 100.0f;
    tank.position = {256, 0, 256};
    world.entities.push_back(tank);
    osc::renderer::Camera camera;
    camera.init(512.0f, 512.0f);
    camera.set_viewport(800.0f, 600.0f);
    camera.set_zoom(40.0f);
    camera.set_pitch(0.87f);
    camera.set_target(256.0f, 256.0f);
    osc::renderer::TextureCache textures;
    const auto quads = [&] {
        osc::renderer::OverlayRenderer overlays;
        osc::sim::WorldEvents events;
        overlays.update(osc::sim::FrameView(&world, &world, 1.0f), events, camera,
                        camera.view_proj(800.0f / 600.0f), nullptr, textures, 800, 600);
        return overlays.quads().size();
    };
    REQUIRE(quads() > 0);
    camera.set_free(true);
    CHECK(quads() == 0);
    camera.set_free(false);
    // Space held across two motions (nearer than zoom 125, where it spins)
    osc::renderer::CameraInput spin;
    spin.space = true;
    spin.mouse_x = 100.0f;
    camera.apply(spin, 0.0);
    spin.mouse_x = 104.0f;
    camera.apply(spin, 0.0);
    REQUIRE(camera.spinning());
    CHECK(quads() == 0);
    camera.apply(osc::renderer::CameraInput{}, 0.0); // let go
    CHECK_FALSE(camera.spinning());
    CHECK(quads() > 0);
}
