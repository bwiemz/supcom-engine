// Quaternions as Moho hands them to scripts (the FAF regression run):
// EulerToQuaternion(roll, pitch, yaw) and OrientFromDir(v) as faf-re shows
// them, carrying the one vector metatable FAF extends with arithmetic.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "blueprints/blueprint_store.hpp"
#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/sim_bindings.hpp"
#include "map/heightmap.hpp"
#include "map/terrain.hpp"
#include "sim/flight_math.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"

extern "C" {
#include <lua.h>
}

#include <array>
#include <bit>
#include <cmath>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

struct SimLua {
    osc::lua::LuaState lua;
    osc::sim::SimState sim{lua.raw(), nullptr};
    SimLua() {
        osc::lua::register_moho_bindings(lua, sim);
        osc::lua::register_sim_bindings(lua, sim);
    }
    /// The quaternion `code` returns, as {x, y, z, w}.
    std::array<double, 4> quat(const std::string& code) {
        auto r = lua.do_string("return " + code);
        INFO((r.ok() ? std::string() : r.error().message));
        REQUIRE(r.ok());
        lua_State* L = lua.raw();
        std::array<double, 4> q{};
        for (int k = 0; k < 4; ++k) {
            lua_rawgeti(L, -1, k + 1);
            q[static_cast<size_t>(k)] = lua_tonumber(L, -1);
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
        return q;
    }
    bool check(const char* code) {
        auto r = lua.do_string(code);
        INFO((r.ok() ? std::string() : r.error().message));
        return r.ok();
    }
};

/// Moho's func_EulerToQuaternion (faf-re MathReflection.cpp).
std::array<double, 4> moho_euler(double roll, double pitch, double yaw) {
    const double cr = std::cos(roll / 2), sr = std::sin(roll / 2);
    const double cp = std::cos(pitch / 2), sp = std::sin(pitch / 2);
    const double cy = std::cos(yaw / 2), sy = std::sin(yaw / 2);
    return {cy * cp * sr - sy * sp * cr, sp * cy * cr + sy * cp * sr, sy * cp * cr - sp * cy * sr,
            cy * cp * cr + sy * sp * sr};
}

/// v rotated by q (x, y, z, w).
std::array<double, 3> rotate(const std::array<double, 4>& q, const std::array<double, 3>& v) {
    const double qx = q[0], qy = q[1], qz = q[2], qw = q[3];
    // t = 2 * (q.xyz x v); v' = v + w t + q.xyz x t
    const double tx = 2 * (qy * v[2] - qz * v[1]);
    const double ty = 2 * (qz * v[0] - qx * v[2]);
    const double tz = 2 * (qx * v[1] - qy * v[0]);
    return {v[0] + qw * tx + (qy * tz - qz * ty), v[1] + qw * ty + (qz * tx - qx * tz),
            v[2] + qw * tz + (qx * ty - qy * tx)};
}

} // namespace

TEST_CASE("EulerToQuaternion takes roll, pitch and yaw, as Moho does", "[quaternion]") {
    SimLua s;
    for (const auto& [r, p, y] : {std::array<double, 3>{0.3, 0.0, 0.0},
                                  {0.0, 0.7, 0.0},
                                  {0.0, 0.0, 1.1},
                                  {0.4, -0.2, 2.5},
                                  {-1.2, 0.9, -0.6}}) {
        const auto got = s.quat("EulerToQuaternion(" + std::to_string(r) + ", " +
                                std::to_string(p) + ", " + std::to_string(y) + ")");
        const auto want = moho_euler(r, p, y);
        INFO("roll " << r << " pitch " << p << " yaw " << y);
        for (size_t k = 0; k < 4; ++k) CHECK(got[k] == Catch::Approx(want[k]).margin(1e-6));
    }
}

TEST_CASE("OrientFromDir faces the direction it is given", "[quaternion]") {
    SimLua s;
    CHECK(s.quat("OrientFromDir(Vector(0, 0, 0))") == std::array<double, 4>{0, 0, 0, 1});
    const double h = std::sqrt(0.5);
    const auto up = s.quat("OrientFromDir(Vector(0, 5, 0))");
    CHECK(up[0] == Catch::Approx(-h));
    CHECK(up[3] == Catch::Approx(h));
    const auto down = s.quat("OrientFromDir(Vector(0, -2, 0))");
    CHECK(down[0] == Catch::Approx(h));
    // Ahead, aside, behind and slanted, through both reachable branches of
    // the matrix conversion: behind and the steep dive take the Y branch.
    for (const auto& d : {std::array<double, 3>{0, 0, 1},
                          {1, 0, 0},
                          {-1, 0, 0},
                          {0, 0, -1},
                          {1, 2, 3},
                          {-3, 0.5, -1},
                          {0.2, -4, -0.1}}) {
        const auto q = s.quat("OrientFromDir(Vector(" + std::to_string(d[0]) + ", " +
                              std::to_string(d[1]) + ", " + std::to_string(d[2]) + "))");
        const double len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        const auto fwd = rotate(q, {0, 0, 1});
        INFO("direction " << d[0] << ", " << d[1] << ", " << d[2]);
        for (size_t k = 0; k < 3; ++k) CHECK(fwd[k] == Catch::Approx(d[k] / len).margin(1e-5));
        CHECK(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3] ==
              Catch::Approx(1.0).margin(1e-5));
    }
}

TEST_CASE("CreatePropHPR takes its heading, pitch and roll in degrees", "[quaternion]") {
    SimLua s;
    const auto orientation = [&](const std::string& hpr) {
        return s.quat("moho.entity_methods.GetOrientation(CreatePropHPR('/p_prop.bp', 0, 0, 0, " +
                      hpr + "))");
    };
    const double h = std::sqrt(0.5);
    const auto expect = [](const std::array<double, 4>& got, const std::array<double, 4>& want) {
        for (size_t k = 0; k < 4; ++k) {
            CHECK(got[k] == Catch::Approx(want[k]).margin(1e-6));
        }
    };
    expect(orientation("0, 0, 0"), {0, 0, 0, 1});
    expect(orientation("90, 0, 0"), {0, h, 0, h});
    expect(orientation("180, 0, 0"), {0, 1, 0, 0});
    expect(orientation("0, 90, 0"), {h, 0, 0, h});
    expect(orientation("0, 0, 90"), {0, 0, h, h});
}

TEST_CASE("CreatePropHPR and CreateUnitHPR turn as retail's, bit for bit", "[quaternion]") {
    // Retail 3599's GetOrientation of each, made at these angles (x, y, z, w).
    struct Case {
        const char* angles;
        osc::u32 want[4];
    };
    SimLua s;
    const auto bits = [](double v) { return std::bit_cast<osc::u32>(static_cast<osc::f32>(v)); };
    for (const Case& c : {Case{"360, 0, 0", {0, 0x33bbbd2e, 0, 0x3f800000}},
                          Case{"180, 0, 0", {0, 0x3f800000, 0, 0xb33bbd2e}},
                          Case{"30, 20, 10", {0x3e41d9eb, 0x3e750aa1, 0x3d1c3301, 0x3f7398af}},
                          Case{"200, -40, 15", {0x3e37fc81, 0x3f68e534, 0x3ea012d8, 0xbe52aea1}}}) {
        const auto q =
            s.quat(std::string(
                       "moho.entity_methods.GetOrientation(CreatePropHPR('/p_prop.bp', 0, 0, 0, ") +
                   c.angles + "))");
        INFO("CreatePropHPR " << c.angles);
        for (size_t k = 0; k < 4; ++k) {
            CHECK(bits(q[k]) == c.want[k]);
        }
    }

    osc::lua::LuaState state;
    osc::blueprints::BlueprintStore store{state.raw()};
    osc::sim::SimState sim{state.raw(), &store};
    osc::lua::register_moho_bindings(state, sim);
    osc::lua::register_sim_bindings(state, sim);
    std::vector<osc::u16> ground(129 * 129, 0);
    sim.set_terrain(std::make_unique<osc::map::Terrain>(
        osc::map::Heightmap(128, 128, 1.0f / 128.0f, std::move(ground)), 0.0f, false));
    sim.add_army("ARMY_1", "ARMY_1");
    lua_State* L = state.raw();
    REQUIRE(state
                .do_string("return {BlueprintId = 'civ', Categories = {'STRUCTURE'},"
                           " Defense = {MaxHealth = 100}, Footprint = {SizeX = 1, SizeZ = 1},"
                           " SizeX = 1, SizeZ = 1}")
                .ok());
    store.register_blueprint(L, osc::blueprints::BlueprintType::Unit, lua_gettop(L));
    lua_pop(L, 1);
    store.expose_to_lua(L);
    osc::f32 x = 0.0f;
    for (const Case& c :
         {Case{"0, 6.283185, 0", {0, 0xb4222169, 0, 0x3f800000}},
          Case{"0.5, 0.3, 0.2", {0x3e840615, 0x3df509a1, 0x3d7113d0, 0x3f74f9da}},
          Case{"0.1, 4.0, -0.2", {0xbde41080, 0x3f66cbd3, 0xbb7426b3, 0xbed60f8e}}}) {
        x += 10.0f;
        REQUIRE(state
                    .do_string("CreateUnitHPR('civ', 'ARMY_1', " + std::to_string(x) + ", 0, 10, " +
                               c.angles + ")")
                    .ok());
        osc::sim::Quaternion q;
        sim.entity_registry().for_each_unit([&](osc::sim::Entity& e) {
            if (e.position().x == x) {
                q = e.orientation();
            }
        });
        INFO("CreateUnitHPR " << c.angles);
        CHECK(bits(q.x) == c.want[0]);
        CHECK(bits(q.y) == c.want[1]);
        CHECK(bits(q.z) == c.want[2]);
        CHECK(bits(q.w) == c.want[3]);
    }
}

TEST_CASE("A map prop turns as retail's map load turns it, bit for bit", "[quaternion]") {
    // Tree groups on SCMP_016: their .scmap rows, and retail 3599's GetOrientation (x, y, z, w).
    struct Case {
        osc::u32 rows[9];
        osc::u32 want[4];
    };
    const auto f = [](osc::u32 b) { return std::bit_cast<osc::f32>(b); };
    for (const Case& c :
         {Case{{0xbecfe854, 0, 0xbf69f18f, 0, 0x3f800000, 0, 0x3f69f18f, 0, 0xbecfe854},
               {0, 0x3f56a614, 0, 0x3f0b8189}},
          Case{{0xbf7feea4, 0, 0x3cbc8286, 0, 0x3f800000, 0, 0xbcbc8286, 0, 0xbf7feea4},
               {0, 0x3f7ffbab, 0, 0xbc3c85ba}}}) {
        const osc::f32 rows[3][3] = {{f(c.rows[0]), f(c.rows[1]), f(c.rows[2])},
                                     {f(c.rows[3]), f(c.rows[4]), f(c.rows[5])},
                                     {f(c.rows[6]), f(c.rows[7]), f(c.rows[8])}};
        const osc::sim::Quaternion q = osc::sim::map_prop_orientation(rows);
        CHECK(std::bit_cast<osc::u32>(q.x) == c.want[0]);
        CHECK(std::bit_cast<osc::u32>(q.y) == c.want[1]);
        CHECK(std::bit_cast<osc::u32>(q.z) == c.want[2]);
        CHECK(std::bit_cast<osc::u32>(q.w) == c.want[3]);
    }
}

TEST_CASE("Vectors and quaternions share the one metatable FAF extends", "[quaternion]") {
    SimLua s;
    CHECK(s.check(R"(
        local mt = getmetatable(Vector2(0, 0))
        assert(mt, 'Vector2 has no metatable')
        assert(getmetatable(Vector(1, 2, 3)) == mt, 'Vector differs')
        assert(getmetatable(EulerToQuaternion(0.1, 0.2, 0.3)) == mt, 'EulerToQuaternion differs')
        assert(getmetatable(OrientFromDir(Vector(1, 0, 0))) == mt, 'OrientFromDir differs')
        -- As FAF's utils.lua does: arithmetic on the shared metatable.
        mt.__mul = function(a, b)
            if getmetatable(a) ~= getmetatable(b) then error('mixed') end
            return 'multiplied'
        end
        assert(EulerToQuaternion(0, 0.2, 0) * OrientFromDir(Vector(0, 0, 1)) == 'multiplied')
        local v = Vector(4, 5, 6)
        assert(v.x == 4 and v.y == 5 and v.z == 6, 'named components')
    )"));
}

TEST_CASE("MATH_Lerp takes Moho's two forms, in either state", "[quaternion]") {
    SimLua s;
    // The UI state has the core functions too (register_core_bindings).
    osc::lua::LuaState ui;
    osc::lua::register_core_bindings(ui);
    const char* code = R"(
        local function near(a, b) return math.abs(a - b) < 1e-5 end
        assert(near(MATH_Lerp(0.25, 10, 20), 12.5), 'three: ' .. MATH_Lerp(0.25, 10, 20))
        -- s from [sMin, sMax] onto [a, b]: FAF's SimPing pulse
        assert(near(MATH_Lerp(0, -0.5, 0.5, 0.3, 0.5), 0.4), 'five')
        assert(near(MATH_Lerp(0.5, 0, 1, 10, 20), 15), 'five: ' .. MATH_Lerp(0.5, 0, 1, 10, 20))
        assert(MATH_Lerp(1, 2, 3, 4) == nil, 'four gives nil')
        assert(not pcall(MATH_Lerp, 1, 2), 'two is an error')
        assert(not pcall(MATH_Lerp, 1, 2, 'x'), 'three are numbers')
    )";
    CHECK(s.check(code));
    auto r = ui.do_string(code);
    INFO((r.ok() ? std::string() : r.error().message));
    CHECK(r.ok());
}
