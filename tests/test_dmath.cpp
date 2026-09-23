// Deterministic math (M197): the sim's transcendental functions give the
// same bits on every platform. The pinned values below come from FDLIBM; CI
// runs this on GCC, Clang and MSVC, so a compiler or flag that changes a
// single bit (FP contraction, a different libm) fails here.

#include <catch2/catch_test_macros.hpp>

#include "core/dmath.hpp"
#include "lua/lua_state.hpp"

extern "C" {
#include <lua.h>
}

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>

namespace {

std::uint64_t bits(double d) {
    std::uint64_t u;
    std::memcpy(&u, &d, sizeof u);
    return u;
}

double call(const std::string& fn, double a, double b) {
    namespace dm = osc::dmath;
    if (fn == "sin") return dm::sin(a);
    if (fn == "cos") return dm::cos(a);
    if (fn == "tan") return dm::tan(a);
    if (fn == "asin") return dm::asin(a);
    if (fn == "acos") return dm::acos(a);
    if (fn == "atan") return dm::atan(a);
    if (fn == "atan2") return dm::atan2(a, b);
    if (fn == "exp") return dm::exp(a);
    if (fn == "log") return dm::log(a);
    if (fn == "log10") return dm::log10(a);
    if (fn == "pow") return dm::pow(a, b);
    if (fn == "hypot") return dm::hypot(a, b);
    FAIL("unknown function " << fn);
    return 0;
}

/// How many representable doubles apart two finite values are.
std::uint64_t ulps_apart(double a, double b) {
    auto ordered = [](double d) {
        const auto u = static_cast<std::int64_t>(bits(d));
        return u < 0 ? std::numeric_limits<std::int64_t>::min() - u : u;
    };
    const std::int64_t x = ordered(a), y = ordered(b);
    return x > y ? static_cast<std::uint64_t>(x - y) : static_cast<std::uint64_t>(y - x);
}

struct Pin {
    const char* fn;
    double a;
    double b;
    std::uint64_t expect;
};

const Pin kPins[] = {
    {"sin", 0.5, 0, 0x3fdeaee8744b05f0ull},
    {"sin", 1, 0, 0x3feaed548f090ceeull},
    {"sin", 2.5, 0, 0x3fe326af0dcfcab0ull},
    {"sin", -3, 0, 0xbfc210386db6d55bull},
    {"sin", 100, 0, 0xbfe03425b78c4db8ull},
    {"sin", 1000000, 0, 0xbfd6664b2568d867ull},
    {"sin", 0.001, 0, 0x3f50624da5218a62ull},
    {"cos", 0.5, 0, 0x3fec1528065b7d50ull},
    {"cos", 1, 0, 0x3fe14a280fb5068cull},
    {"cos", 2.5, 0, 0xbfe9a2f7ef858b7dull},
    {"cos", -3, 0, 0xbfefae04be85e5d2ull},
    {"cos", 100, 0, 0x3feb981dbf665fdfull},
    {"cos", 1000000, 0, 0x3fedf9df9906d32cull},
    {"cos", 0.001, 0, 0x3feffffef390876cull},
    {"tan", 0.5, 0, 0x3fe17b4f5bf3474aull},
    {"tan", 1, 0, 0x3ff8eb245cbee3a6ull},
    {"tan", 2.5, 0, 0xbfe7e79b4e00bb15ull},
    {"tan", -3, 0, 0x3fc23ef71254b86full},
    {"tan", 100, 0, 0xbfe2ca74d62b5d38ull},
    {"tan", 1000000, 0, 0xbfd7e9768ab734c0ull},
    {"tan", 0.001, 0, 0x3f50624e2e91ebe4ull},
    {"atan", 0.5, 0, 0x3fddac670561bb4full},
    {"atan", 1, 0, 0x3fe921fb54442d18ull},
    {"atan", 2.5, 0, 0x3ff30b6d796a4da8ull},
    {"atan", -3, 0, 0xbff3fc176b7a8560ull},
    {"atan", 100, 0, 0x3ff8f905eb2def22ull},
    {"atan", 1000000, 0, 0x3ff921fa47d4b30dull},
    {"atan", 0.001, 0, 0x3f50624d77516e16ull},
    {"exp", 0.5, 0, 0x3ffa61298e1e069cull},
    {"exp", 1, 0, 0x4005bf0a8b14576aull},
    {"exp", 2.5, 0, 0x40285d6fd931e0bbull},
    {"exp", -3, 0, 0x3fa97db0ccceb0afull},
    {"exp", 100, 0, 0x48f3494a9b171bf5ull},
    {"exp", 1000000, 0, 0x7ff0000000000000ull},
    {"exp", 0.001, 0, 0x3ff0041919b7ee34ull},
    {"asin", -0.90000000000000002, 0, 0xbff1ea93705fa172ull},
    {"asin", -0.25, 0, 0xbfd02be9ce0b87cdull},
    {"asin", 0.10000000000000001, 0, 0x3fb9a49276037884ull},
    {"asin", 0.5, 0, 0x3fe0c152382d7366ull},
    {"asin", 0.98999999999999999, 0, 0x3ff6de3c6f33d51dull},
    {"acos", -0.90000000000000002, 0, 0x400586476251e745ull},
    {"acos", -0.25, 0, 0x3ffd2cf5c7c70f0cull},
    {"acos", 0.10000000000000001, 0, 0x3ff787b22ce3f590ull},
    {"acos", 0.5, 0, 0x3ff0c152382d7366ull},
    {"acos", 0.98999999999999999, 0, 0x3fc21df72882bfd8ull},
    {"log", 0.001, 0, 0xc01ba18a998fffa0ull},
    {"log", 0.5, 0, 0xbfe62e42fefa39efull},
    {"log", 2, 0, 0x3fe62e42fefa39efull},
    {"log", 10, 0, 0x40026bb1bbb55516ull},
    {"log", 12345.678, 0, 0x4022d79559791e31ull},
    {"log10", 0.001, 0, 0xc008000000000000ull},
    {"log10", 0.5, 0, 0xbfd34413509f79ffull},
    {"log10", 2, 0, 0x3fd34413509f79ffull},
    {"log10", 10, 0, 0x3ff0000000000000ull},
    {"log10", 12345.678, 0, 0x40105db618083a9eull},
    {"atan2", 1, 1, 0x3fe921fb54442d18ull},
    {"hypot", 1, 1, 0x3ff6a09e667f3bcdull},
    {"atan2", -2.5, 0.29999999999999999, 0xbff738cd060d0db1ull},
    {"hypot", -2.5, 0.29999999999999999, 0x400424bb73db9e40ull},
    {"atan2", 0.69999999999999996, -4, 0x4007bf2cb4877bacull},
    {"hypot", 0.69999999999999996, -4, 0x40103e3f3c64894dull},
    {"atan2", 3, 4, 0x3fe4978fa3269ee1ull},
    {"hypot", 3, 4, 0x4014000000000000ull},
    {"atan2", 1.5, 2.25, 0x3fe2d0ead6066395ull},
    {"hypot", 1.5, 2.25, 0x4005a22073490377ull},
    {"pow", 2, 0.5, 0x3ff6a09e667f3bcdull},
    {"pow", 1.5, 2.25, 0x4003eb971cfb5f72ull},
    {"pow", 10, -3, 0x3f50624dd2f1a9fcull},
    {"pow", 0.29999999999999999, 7.0999999999999996, 0x3f2969f38095a047ull},
    {"pow", 123.40000000000001, 1.7, 0x40ac0e56392a25a9ull},
};

} // namespace

TEST_CASE("dmath gives FDLIBM's exact bits", "[dmath]") {
    for (const Pin& p : kPins) {
        INFO(p.fn << "(" << p.a << ", " << p.b << ")");
        CHECK(bits(call(p.fn, p.a, p.b)) == p.expect);
    }
}

TEST_CASE("dmath agrees with the system libm to within an ulp or two", "[dmath]") {
    // Both are within 1 ulp of the true value, so at most 2 apart.
    for (int i = -400; i <= 400; ++i) {
        const double x = i * 0.0625 + 0.013;
        CHECK(ulps_apart(osc::dmath::sin(x), std::sin(x)) <= 2);
        CHECK(ulps_apart(osc::dmath::cos(x), std::cos(x)) <= 2);
        CHECK(ulps_apart(osc::dmath::atan(x), std::atan(x)) <= 2);
        CHECK(ulps_apart(osc::dmath::atan2(x, 1.7), std::atan2(x, 1.7)) <= 2);
        CHECK(ulps_apart(osc::dmath::hypot(x, 3.1), std::hypot(x, 3.1)) <= 2);
        if (std::fabs(x) < 20) CHECK(ulps_apart(osc::dmath::exp(x), std::exp(x)) <= 2);
        if (x > 0) {
            CHECK(ulps_apart(osc::dmath::log(x), std::log(x)) <= 2);
            CHECK(ulps_apart(osc::dmath::log10(x), std::log10(x)) <= 2);
            CHECK(ulps_apart(osc::dmath::pow(x, 1.37), std::pow(x, 1.37)) <= 2);
        }
        if (std::fabs(x) <= 1) {
            CHECK(ulps_apart(osc::dmath::asin(x), std::asin(x)) <= 2);
            CHECK(ulps_apart(osc::dmath::acos(x), std::acos(x)) <= 2);
        }
    }
    // IEEE special cases.
    CHECK(std::isnan(osc::dmath::log(-1.0)));
    CHECK(osc::dmath::exp(-1000.0) == 0.0);
    CHECK(std::isinf(osc::dmath::exp(1000.0)));
    CHECK(osc::dmath::pow(0.0, 0.0) == 1.0);
}

TEST_CASE("Scripts' math library and ^ use the same functions", "[dmath][lua]") {
    osc::lua::LuaState lua;
    auto number = [&](const char* code) {
        auto r = lua.do_string(code);
        INFO((r.ok() ? std::string() : r.error().message));
        REQUIRE(r.ok());
        const double v = lua_tonumber(lua.raw(), -1);
        lua_pop(lua.raw(), 1);
        return v;
    };
    CHECK(bits(number("return math.sin(1.3)")) == bits(osc::dmath::sin(1.3)));
    CHECK(bits(number("return math.atan2(0.3, -2)")) == bits(osc::dmath::atan2(0.3, -2.0)));
    CHECK(bits(number("return math.pow(1.5, 2.25)")) == bits(osc::dmath::pow(1.5, 2.25)));
    CHECK(bits(number("return 1.5 ^ 2.25")) == bits(osc::dmath::pow(1.5, 2.25)));
    CHECK(bits(number("return math.exp(0.7)")) == bits(osc::dmath::exp(0.7)));
    CHECK(bits(number("return math.log10(12345.678)")) == bits(osc::dmath::log10(12345.678)));
}

TEST_CASE("Lua turns numbers into the same strings everywhere", "[dmath][lua]") {
    osc::lua::LuaState lua;
    auto text = [&](const char* code) {
        auto r = lua.do_string(code);
        INFO((r.ok() ? std::string() : r.error().message));
        REQUIRE(r.ok());
        std::string v = lua_tostring(lua.raw(), -1);
        lua_pop(lua.raw(), 1);
        return v;
    };
    // "%.14g", spelled out.
    CHECK(text("return tostring(0.1)") == "0.1");
    CHECK(text("return tostring(1/3)") == "0.33333333333333");
    CHECK(text("return tostring(42)") == "42");
    CHECK(text("return tostring(-2.5)") == "-2.5");
    CHECK(text("return tostring(1e21)") == "1e+21");
    CHECK(text("return tostring(1.5e-7)") == "1.5e-07");
    CHECK(text("return tostring(123456.789)") == "123456.789");
    CHECK(text("return 'x' .. 0.25") == "x0.25");
    CHECK(text("return tostring(1/0)") == "inf");
    CHECK(text("return tostring(-1/0)") == "-inf");

    // And the same as the C library's "%.14g" wherever that is well defined.
    for (double v : {3.14159265358979, 2.0 / 3.0, 1e-300, 6.02214076e23, -0.0078125}) {
        char expect[64];
        std::snprintf(expect, sizeof expect, "%.14g", v);
        lua_pushnumber(lua.raw(), v);
        CHECK(std::string(lua_tostring(lua.raw(), -1)) == expect);
        lua_pop(lua.raw(), 1);
    }
}
