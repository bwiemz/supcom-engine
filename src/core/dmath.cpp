#include "core/dmath.hpp"

#include "osc_fdlibm.h"

namespace osc::dmath {

double sin(double x) {
    return ieee_sin(x);
}
double cos(double x) {
    return ieee_cos(x);
}
double tan(double x) {
    return ieee_tan(x);
}
double asin(double x) {
    return ieee_asin(x);
}
double acos(double x) {
    return ieee_acos(x);
}
double atan(double x) {
    return ieee_atan(x);
}
double atan2(double y, double x) {
    return ieee_atan2(y, x);
}
double exp(double x) {
    return ieee_exp(x);
}
double log(double x) {
    return ieee_log(x);
}
double log10(double x) {
    return ieee_log10(x);
}
double pow(double x, double y) {
    return ieee_pow(x, y);
}
double hypot(double x, double y) {
    return ieee_hypot(x, y);
}

} // namespace osc::dmath
