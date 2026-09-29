#include <catch2/catch_test_macros.hpp>

#include "renderer/playable_rect.hpp"
#include "sim/world_snapshot.hpp"

using namespace osc;
using renderer::clamp_playable_rect;
using renderer::PlayableRect;
using renderer::UserPlayableRect;

namespace {

bool same(const std::optional<PlayableRect>& r, i32 x0, i32 z0, i32 x1, i32 z1) {
    return r && r->x0 == x0 && r->z0 == z0 && r->x1 == x1 && r->z1 == z1;
}

void add(sim::WorldSnapshot& snap, u32 id, f32 x, f32 z) {
    sim::EntityRecord e;
    e.id = id;
    e.position = {x, 0, z};
    snap.entities.push_back(e);
}

} // namespace

TEST_CASE("A playable rect is kept to the map, as the sim's is", "[playable_rect]") {
    CHECK(same(clamp_playable_rect({64, 32, 448, 480}, 512, 512), 64, 32, 448, 480));
    // Each edge within [0, size]
    CHECK(same(clamp_playable_rect({-20, -1, 600, 1024}, 512, 256), 0, 0, 512, 256));
    CHECK(same(clamp_playable_rect({0, 0, 512, 512}, 512, 512), 0, 0, 512, 512));
    // Nothing left: empty, inverted, off the map, or no map
    CHECK_FALSE(clamp_playable_rect({100, 100, 100, 200}, 512, 512));
    CHECK_FALSE(clamp_playable_rect({200, 100, 100, 200}, 512, 512));
    CHECK_FALSE(clamp_playable_rect({600, 0, 700, 100}, 512, 512));
    CHECK_FALSE(clamp_playable_rect({-50, 0, -10, 100}, 512, 512));
    CHECK_FALSE(clamp_playable_rect({0, 0, 100, 100}, 0, 512));
}

TEST_CASE("A playable rect sync hides the meshes then outside it, until the next",
          "[playable_rect]") {
    sim::WorldSnapshot snap;
    add(snap, 1, 10, 10);    // inside
    add(snap, 2, 99.9f, 50); // inside: whole units, x < x1
    add(snap, 3, 100, 50);   // outside: x1 is not in it
    add(snap, 4, -0.5f, 10); // inside: truncated to 0, as Moho does
    add(snap, 5, 150, 150);  // outside
    add(snap, 6, 50, 100);   // outside: z1 is not in it
    const sim::FrameView view(&snap, &snap, 1.0f);

    UserPlayableRect playable;
    playable.sync({0, 0, 100, 100});
    for (u32 id = 1; id <= 6; ++id) CHECK_FALSE(playable.hides(id)); // until a frame applies it
    playable.apply(sim::FrameView());                                // (a frame with no world)
    CHECK_FALSE(playable.hides(3));

    playable.apply(view);
    CHECK_FALSE(playable.hides(1));
    CHECK_FALSE(playable.hides(2));
    CHECK(playable.hides(3));
    CHECK_FALSE(playable.hides(4));
    CHECK(playable.hides(5));
    CHECK(playable.hides(6));

    // One that moves in stays hidden, one that appears later is drawn.
    snap.entities[4].position = {50, 0, 50};
    add(snap, 7, 300, 300);
    playable.apply(view); // nothing pending: nothing changes
    CHECK(playable.hides(5));
    CHECK_FALSE(playable.hides(7));

    // The next sync decides afresh.
    playable.sync({0, 0, 512, 512});
    playable.apply(view);
    for (u32 id = 1; id <= 7; ++id) CHECK_FALSE(playable.hides(id));

    // A new world: nothing hidden, the pending sync dropped.
    playable.sync({0, 0, 10, 10});
    playable.clear();
    playable.apply(view);
    CHECK_FALSE(playable.hides(5));
}
