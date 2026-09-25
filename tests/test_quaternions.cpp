// Quaternions as Moho hands them to scripts (the FAF regression run):
// EulerToQuaternion(roll, pitch, yaw) and OrientFromDir(v) as faf-re shows
// them, carrying the one vector metatable FAF extends with arithmetic.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/sim_bindings.hpp"
#include "sim/sim_state.hpp"

extern "C" {
#include <lua.h>
}

#include <array>
#include <cmath>
#include <string>

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
