// Timed effects (decals, splats, beams given a duration) expire after their
// LIFETIME, counted from their birth; the rest last until destroyed.

#include <catch2/catch_test_macros.hpp>

#include "sim/ieffect.hpp"

#include <cmath>

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

TEST_CASE("Emitters end when their emission does", "[effects]") {
    IEffectRegistry reg;
    auto* once = reg.create();
    once->set_ends_at(12.0);
    auto* looping = reg.create();        // no end: emits on
    auto* lifetime_param = reg.create(); // an emitter's end, not a decal's LIFETIME
    lifetime_param->set_param("LIFETIME", 1.0);
    lifetime_param->set_ends_at(20.0);

    reg.expire_timed(11.9);
    CHECK_FALSE(once->destroyed());
    reg.expire_timed(12.0);
    CHECK(once->destroyed());
    CHECK_FALSE(looping->destroyed());
    CHECK_FALSE(lifetime_param->destroyed());
    reg.expire_timed(1000.0);
    CHECK_FALSE(looping->destroyed());
    CHECK(lifetime_param->destroyed());
}

TEST_CASE("Effects that follow an entity go with it; placed ones stay", "[effects]") {
    using osc::sim::EffectType;
    IEffectRegistry reg;
    const auto make = [&](EffectType type, osc::u32 entity) {
        auto* fx = reg.create();
        fx->set_type(type);
        fx->set_entity_id(entity);
        return fx;
    };
    auto* on_entity = make(EffectType::ATTACHED_EMITTER, 7);
    auto* trail = make(EffectType::TRAIL_EMITTER, 7);
    auto* beam = make(EffectType::ATTACHED_BEAM, 7);
    auto* at_entity = make(EffectType::EMITTER_AT_ENTITY, 7);
    auto* at_bone = make(EffectType::EMITTER_AT_BONE, 7);
    auto* other = make(EffectType::ATTACHED_EMITTER, 8);
    auto* unowned = make(EffectType::ATTACHED_EMITTER, 0);

    reg.destroy_detached([](osc::u32 id) { return id == 7 || id == 0; });
    CHECK(on_entity->destroyed());
    CHECK(trail->destroyed());
    CHECK(beam->destroyed());
    CHECK_FALSE(at_entity->destroyed());
    CHECK_FALSE(at_bone->destroyed());
    CHECK_FALSE(other->destroyed());
    CHECK_FALSE(unowned->destroyed());
}

TEST_CASE("An emitter blueprint's Lifetime is read once", "[effects]") {
    IEffectRegistry reg;
    int computed = 0;
    const auto compute = [&] {
        ++computed;
        return 20.0;
    };
    CHECK(reg.blueprint_lifetime("/a.bp", compute) == 20.0);
    CHECK(reg.blueprint_lifetime("/a.bp", compute) == 20.0);
    CHECK(computed == 1);
    CHECK(reg.blueprint_lifetime("/b.bp", [] { return -1.0; }) == -1.0);
    CHECK(computed == 1);
}

TEST_CASE("An emitter emits for its Lifetime in whole ticks, as Moho counts", "[effects]") {
    using osc::sim::emitter_life_ticks;
    CHECK(emitter_life_ticks(20.0) == 20.0);
    CHECK(emitter_life_ticks(0.1) == 1.0); // a muzzle flash: one tick
    CHECK(emitter_life_ticks(1.5) == 2.0);
    CHECK(emitter_life_ticks(0.0) == 0.0);   // ends before it emits
    CHECK(emitter_life_ticks(-1.0) == -1.0); // emits on
    CHECK(emitter_life_ticks(std::nan("")) == -1.0);

    // It ends on the tick it runs out, exactly: game time is the tick count
    // times the tick length, and so is the end.
    constexpr double kTick = 0.1;
    IEffectRegistry reg;
    auto* fx = reg.create();
    fx->set_created_tick(7);
    fx->end_after(20.0, kTick);
    CHECK(fx->ends_at() == 27u * kTick);
    reg.expire_timed(26u * kTick);
    CHECK_FALSE(fx->destroyed());
    reg.expire_timed(27u * kTick);
    CHECK(fx->destroyed());

    auto* forever = reg.create();
    forever->set_created_tick(7);
    forever->end_after(-1.0, kTick);
    CHECK(forever->ends_at() < 0);
    auto* at_once = reg.create();
    at_once->set_created_tick(7);
    at_once->end_after(0.0, kTick);
    reg.expire_timed(7u * kTick);
    CHECK(at_once->destroyed());
    CHECK_FALSE(forever->destroyed());
}
