// Timed effects (decals, splats, beams given a duration) expire after their
// LIFETIME, counted from their birth; the rest last until destroyed.

#include <catch2/catch_test_macros.hpp>

#include "sim/ieffect.hpp"

using osc::sim::IEffectRegistry;

TEST_CASE("Timed effects expire after their LIFETIME", "[effects]") {
    IEffectRegistry reg;
    auto* timed = reg.create();
    timed->set_param("LIFETIME", 2.0);
    timed->set_birth_time(10.0);
    auto* forever = reg.create(); // no birth time: no auto-expiry
    forever->set_param("LIFETIME", 1.0);
    auto* untimed = reg.create(); // born, but no LIFETIME
    untimed->set_birth_time(10.0);
    auto* other_case = reg.create(); // param names are exact
    other_case->set_param("Lifetime", 1.0);
    other_case->set_birth_time(10.0);
    auto* retimed = reg.create(); // the last LIFETIME set counts
    retimed->set_param("LIFETIME", 1.0);
    retimed->set_param("LIFETIME", 5.0);
    retimed->set_birth_time(10.0);

    CHECK(timed->lifetime() == timed->get_param("LIFETIME"));
    CHECK(untimed->lifetime() == 0.0);

    reg.expire_timed(11.9);
    CHECK_FALSE(timed->destroyed());
    reg.expire_timed(12.0);
    CHECK(timed->destroyed());
    CHECK_FALSE(forever->destroyed());
    CHECK_FALSE(untimed->destroyed());
    CHECK_FALSE(other_case->destroyed());
    CHECK_FALSE(retimed->destroyed());
    reg.expire_timed(15.0);
    CHECK(retimed->destroyed());
    reg.gc();
    CHECK(reg.count() == 3);
}
