/* The FDLIBM functions OpenSupCom calls: bit-identical results on every
 * platform (see README.osc). A clean header for C++ callers -- fdlibm.h
 * itself defines macros (HUGE, DOMAIN, __P, ...) that would leak. */
#ifndef OSC_FDLIBM_H
#define OSC_FDLIBM_H

#ifdef __cplusplus
extern "C" {
#endif

double ieee_sin(double x);
double ieee_cos(double x);
double ieee_tan(double x);
double ieee_asin(double x);
double ieee_acos(double x);
double ieee_atan(double x);
double ieee_atan2(double y, double x);
double ieee_exp(double x);
double ieee_log(double x);
double ieee_log10(double x);
double ieee_pow(double x, double y);
double ieee_hypot(double x, double y);

#ifdef __cplusplus
}
#endif

#endif /* OSC_FDLIBM_H */
