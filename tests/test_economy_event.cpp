#include <catch2/catch_test_macros.hpp>

#include "sim/economy_event.hpp"

using osc::sim::EconomyEvent;
using osc::sim::EconomyEventRegistry;

TEST_CASE("An economy event draws its cost over its time", "[economy_event]") {
    EconomyEvent evt(1, 50.0, 1000.0, 1.0);
    CHECK(evt.mass_per_second() == 50.0);
    CHECK(evt.energy_per_second() == 1000.0);
    for (int i = 0; i < 9; ++i) evt.advance(0.1, 1.0);
    CHECK_FALSE(evt.is_done());
    evt.advance(0.1, 1.0);
    CHECK(evt.is_done());
    CHECK(evt.progress() == 1.0);
}

TEST_CASE("A stalled economy event moves on as far as it was paid", "[economy_event]") {
    EconomyEvent evt(1, 0.0, 100.0, 1.0);
    for (int i = 0; i < 10; ++i) evt.advance(0.1, 0.5);
    CHECK_FALSE(evt.is_done());
    CHECK(evt.progress() > 0.49);
    CHECK(evt.progress() < 0.51);
    for (int i = 0; i < 10; ++i) evt.advance(0.1, 0.5);
    CHECK(evt.is_done());
}

TEST_CASE("An economy event asks for a tick's worth at least", "[economy_event]") {
    // Retail makes zero-time events (a weapon's 0.1 s floor aside): they
    // take a tick, not a division by zero.
    EconomyEvent evt(1, 0.0, 10.0, 0.0);
    CHECK(evt.energy_per_second() == 100.0);
    evt.advance(0.1, 1.0);
    CHECK(evt.is_done());
}

TEST_CASE("Events made during a pass wait for the next", "[economy_event]") {
    // A progress callback may make events; enough of them move the list.
    // Only the first visit makes them: a pass that visited new events would
    // otherwise make more without end, filling memory instead of failing.
    EconomyEventRegistry registry;
    registry.create(1, 0.0, 10.0, 1.0);
    int visits = 0;
    registry.for_each([&](EconomyEvent&) {
        if (++visits > 1) return;
        for (int i = 0; i < 64; ++i) registry.create(2, 0.0, 1.0, 1.0);
    });
    CHECK(visits == 1);
    CHECK(registry.count() == 65);

    visits = 0;
    registry.for_each([&](EconomyEvent&) { ++visits; });
    CHECK(visits == 65);
}

TEST_CASE("Finished and cancelled events are collected", "[economy_event]") {
    EconomyEventRegistry registry;
    EconomyEvent* done = registry.create(1, 0.0, 1.0, 0.1);
    EconomyEvent* cancelled = registry.create(1, 0.0, 1.0, 1.0);
    registry.create(1, 0.0, 1.0, 1.0);
    done->advance(0.1, 1.0);
    cancelled->cancel();
    CHECK_FALSE(cancelled->active());
    registry.gc();
    CHECK(registry.count() == 1);
}
