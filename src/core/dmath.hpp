#pragma once

#include "core/types.hpp"

/// Deterministic math for the sim: sin, atan2, pow and the rest computed by
/// FDLIBM from IEEE-754 arithmetic alone, so every platform gets the same
/// bits (the system libms differ in the last one, and in a lockstep game a
/// last-bit difference in a heading grows into a desync). sqrt, floor, ceil,
/// fmod and fabs need no replacement: IEEE-754 defines them exactly.
///
/// Sim code (and anything that feeds sim state) calls these; the renderer
/// and UI may use <cmath>.
namespace osc::dmath {

double sin(double x);
double cos(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);
double exp(double x);
double log(double x);
double log10(double x);
double pow(double x, double y);
double hypot(double x, double y);

// The sim's f32 math, computed in double and rounded once to float.
inline f32 sin(f32 x) {
    return static_cast<f32>(sin(static_cast<double>(x)));
}
inline f32 cos(f32 x) {
    return static_cast<f32>(cos(static_cast<double>(x)));
}
inline f32 tan(f32 x) {
    return static_cast<f32>(tan(static_cast<double>(x)));
}
inline f32 asin(f32 x) {
    return static_cast<f32>(asin(static_cast<double>(x)));
}
inline f32 acos(f32 x) {
    return static_cast<f32>(acos(static_cast<double>(x)));
}
inline f32 atan(f32 x) {
    return static_cast<f32>(atan(static_cast<double>(x)));
}
inline f32 atan2(f32 y, f32 x) {
    return static_cast<f32>(atan2(static_cast<double>(y), static_cast<double>(x)));
}
inline f32 hypot(f32 x, f32 y) {
    return static_cast<f32>(hypot(static_cast<double>(x), static_cast<double>(y)));
}

} // namespace osc::dmath
