#include <catch2/catch_test_macros.hpp>

#include "sim/emitter_params.hpp"
#include "sim/ieffect.hpp"

using namespace osc::sim;

TEST_CASE("Emitter params and curves by Moho's names", "[sim][emitter]") {
    CHECK(emitter_param("LIFETIME") == kParamLifetime);
    CHECK(emitter_param("lifetime") == kParamLifetime);
    CHECK(emitter_param("EFFECT_REPEATTIME") == kParamRepeatTime);
    CHECK(emitter_param("POSITION") == kParamPositionX);
    CHECK(emitter_param("position_z") == kParamPositionZ);
    CHECK(emitter_param("ParticleResistance") == kParamParticleResistance);
    CHECK_FALSE(emitter_param("THICKNESS")); // a beam's, not an emitter's
    CHECK_FALSE(emitter_param(""));
    CHECK(emitter_param_name(kParamRepeatTime) == "REPEATTIME");

    CHECK(emitter_curve("X_POSITION_CURVE") == 11);
    CHECK(emitter_curve("emitter_z_position_curve") == 13);
    CHECK(emitter_curve("EMITRATE_CURVE") == 3);
    CHECK(emitter_curve("RAMPSELECTION_CURVE") == kEmitterCurves - 1);
    CHECK_FALSE(emitter_curve("X_POSITION"));
}

TEST_CASE("An effect's curve calls: a set replaces its curve's, a resize the last resize",
          "[sim][emitter]") {
    IEffect fx;
    CHECK(fx.overrides_serial() == 0);
    fx.add_curve_op({11, true, 8, 0});
    fx.add_curve_op({11, true, 12, 0}); // resizing twice is resizing once
    fx.add_curve_op({13, false, 0, 3});
    REQUIRE(fx.curve_ops().size() == 2);
    CHECK(fx.curve_ops()[0].a == 12);
    fx.add_curve_op({11, false, 0, 4.5f}); // a set drops the resize before it
    REQUIRE(fx.curve_ops().size() == 2);
    CHECK(fx.curve_ops()[1].curve == 11);
    CHECK_FALSE(fx.curve_ops()[1].resize);
    CHECK(fx.overrides_serial() == 4);

    fx.set_emitter_param(kParamRepeatTime, 2);
    CHECK(fx.emitter_params_set() == 1u << kParamRepeatTime);
    CHECK(fx.emitter_param(kParamRepeatTime) == 2.0f);
    CHECK(fx.overrides_serial() == 5);
}
