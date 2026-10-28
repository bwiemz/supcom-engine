// A projectile's terrain bounce, as Moho's (faf-re Projectile.cpp): it picks
// MinBounceCount to MaxBounceCount as it is made, and a ground hit within that
// count leaves with its damped velocity reflected about the ground's normal
// instead of ending it. Retail's nuke shockwave effects bounce.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "lua/lua_state.hpp"
#include "map/heightmap.hpp"
#include "map/terrain.hpp"
#include "sim/entity_registry.hpp"
#include "sim/projectile.hpp"
#include "sim/sim_random.hpp"

#include <memory>
#include <string>
#include <vector>

using Catch::Approx;
using osc::f32;
using osc::sim::EntityRegistry;
using osc::sim::Projectile;

TEST_CASE("A projectile reads its bounces from its blueprint, Moho's defaults 0, 0 and 0.5",
          "[projectile][bounce]") {
    osc::lua::LuaState state;
    REQUIRE(state
                .do_string(R"(
        __blueprints = {
            shock = {Physics = {MinBounceCount = 3, MaxBounceCount = 3, BounceVelDamp = 0.8}},
            spread = {Physics = {MinBounceCount = 1, MaxBounceCount = 5}},
            plain = {Physics = {}},
        }
    )")
                .ok());
    Projectile shock;
    shock.set_blueprint_id("shock");
    (void)shock.apply_blueprint_physics(state.raw());
    CHECK(shock.bounce_limit == 3);
    CHECK(shock.bounce_damping == Approx(0.8f));
    Projectile plain;
    plain.set_blueprint_id("plain");
    (void)plain.apply_blueprint_physics(state.raw());
    CHECK(plain.bounce_limit == 0);
    CHECK(plain.bounce_damping == Approx(0.5f));

    // A span is one draw scaled into [Min, Max)
    osc::sim::SimRandom rng;
    bool low = false;
    bool high = false;
    for (int i = 0; i < 200; ++i) {
        Projectile p;
        p.set_blueprint_id("spread");
        (void)p.apply_blueprint_physics(state.raw(), &rng);
        REQUIRE(p.bounce_limit >= 1);
        REQUIRE(p.bounce_limit <= 4);
        low = low || p.bounce_limit == 1;
        high = high || p.bounce_limit == 4;
    }
    CHECK(low);
    CHECK(high);
}

TEST_CASE("A projectile bounces off the ground as often as its limit, then impacts",
          "[projectile][bounce]") {
    // Flat ground at 7.8125: its normal is straight up, so a bounce sends the damped velocity back
    // up, mirrored (Moho reflects about CHeightField::GetNormal).
    osc::map::Heightmap hm(64, 64, 1.0f / 128.0f, std::vector<osc::u16>(65 * 65, 1000));
    const osc::map::Terrain terrain(std::move(hm), 0.0f, false);
    EntityRegistry reg;
    reg.init_spatial_grid(64, 64);
    auto shot = std::make_unique<Projectile>();
    shot->set_position({20.0f, 9.0f, 20.0f});
    shot->velocity = {10.0f, -20.0f, 0.0f};
    shot->ballistic_accel = -Projectile::GRAVITY;
    shot->bounce_limit = 2;
    shot->bounce_damping = 0.5f;
    auto* p = static_cast<Projectile*>(reg.find(reg.register_entity(std::move(shot))));
    const osc::u32 id = p->entity_id();

    std::vector<osc::sim::Vector3> left_with; // the velocity each bounce left with
    for (osc::u32 tick = 1; tick < 200; ++tick) {
        if (!reg.find(id) || reg.find(id)->destroyed()) break;
        const bool was_pending = p->bounce_pending;
        p->update(0.1, reg, nullptr, &terrain, tick);
        if (was_pending) left_with.push_back(p->velocity);
        reg.advance_coords();
    }
    // Two bounces, each up and away at half the speed it came down with, then an impact on the
    // third ground hit
    REQUIRE(left_with.size() == 2);
    CHECK(left_with[0].y > 0.0f);
    CHECK(left_with[0].x == Approx(5.0f).margin(0.5f));
    CHECK(left_with[1].y > 0.0f);
    CHECK(left_with[1].y < left_with[0].y);
    const auto* gone = reg.find(id);
    CHECK((!gone || gone->destroyed()));
    CHECK(p->ground_hits == 3);
}

TEST_CASE("A ground hit is a Terrain impact where it crosses the ground, though its step ends over "
          "the water",
          "[projectile][bounce]") {
    std::vector<osc::u16> heights(65 * 65, 1536);
    for (int z = 0; z < 65; ++z) {
        for (int x = 33; x < 65; ++x) {
            heights[static_cast<size_t>(z) * 65 + static_cast<size_t>(x)] = 640;
        }
    }
    osc::map::Heightmap hm(64, 64, 1.0f / 128.0f, std::move(heights));
    const osc::map::Terrain terrain(std::move(hm), 8.0f, true);
    EntityRegistry reg;
    reg.init_spatial_grid(64, 64);
    auto shot = std::make_unique<Projectile>();
    shot->set_position({30.5f, 12.2f, 20.0f});
    shot->velocity = {30.0f, -10.0f, 0.0f};
    auto* p = static_cast<Projectile*>(reg.find(reg.register_entity(std::move(shot))));
    p->update(0.1, reg, nullptr, &terrain, 1);
    REQUIRE(p->impact_waiting());
    CHECK(p->impact_kind == "Terrain");
    CHECK(p->pending_position().x < 32.0f);
}

TEST_CASE("A projectile bounces off the seabed too, and its hit there is a Terrain impact",
          "[projectile][bounce]") {
    osc::map::Heightmap hm(64, 64, 1.0f / 128.0f, std::vector<osc::u16>(65 * 65, 512));
    const osc::map::Terrain terrain(std::move(hm), 10.0f, true);
    EntityRegistry reg;
    reg.init_spatial_grid(64, 64);
    auto shot = std::make_unique<Projectile>();
    shot->set_position({20.0f, 9.0f, 20.0f});
    shot->velocity = {2.0f, -5.0f, 0.0f};
    shot->ballistic_accel = -Projectile::GRAVITY;
    shot->bounce_limit = 2;
    auto* p = static_cast<Projectile*>(reg.find(reg.register_entity(std::move(shot))));
    const osc::u32 id = p->entity_id();
    int bounces = 0;
    std::string kind;
    for (osc::u32 tick = 1; tick < 200; ++tick) {
        if (!reg.find(id) || reg.find(id)->destroyed()) {
            break;
        }
        const bool was_pending = p->bounce_pending;
        p->update(0.1, reg, nullptr, &terrain, tick);
        if (was_pending) {
            ++bounces;
        }
        if (p->impact_waiting()) {
            kind = p->impact_kind;
        }
        reg.advance_coords();
    }
    CHECK(bounces == 2);
    CHECK(kind == "Terrain");
}
