/* OpenSupCom: the library's internal names, prefixed so they can never
 * collide with (or interpose on) a system libm's internals. Included at
 * the top of fdlibm.h. */
#ifndef OSC_FDLIBM_NAMES_H
#define OSC_FDLIBM_NAMES_H
#define __ieee754_acos osc_fdlibm_ieee754_acos
#define __ieee754_acosh osc_fdlibm_ieee754_acosh
#define __ieee754_asin osc_fdlibm_ieee754_asin
#define __ieee754_atan2 osc_fdlibm_ieee754_atan2
#define __ieee754_atanh osc_fdlibm_ieee754_atanh
#define __ieee754_cosh osc_fdlibm_ieee754_cosh
#define __ieee754_exp osc_fdlibm_ieee754_exp
#define __ieee754_fmod osc_fdlibm_ieee754_fmod
#define __ieee754_gamma osc_fdlibm_ieee754_gamma
#define __ieee754_gamma_r osc_fdlibm_ieee754_gamma_r
#define __ieee754_hypot osc_fdlibm_ieee754_hypot
#define __ieee754_j0 osc_fdlibm_ieee754_j0
#define __ieee754_j1 osc_fdlibm_ieee754_j1
#define __ieee754_jn osc_fdlibm_ieee754_jn
#define __ieee754_lgamma osc_fdlibm_ieee754_lgamma
#define __ieee754_lgamma_r osc_fdlibm_ieee754_lgamma_r
#define __ieee754_log osc_fdlibm_ieee754_log
#define __ieee754_log10 osc_fdlibm_ieee754_log10
#define __ieee754_pow osc_fdlibm_ieee754_pow
#define __ieee754_rem_pio2 osc_fdlibm_ieee754_rem_pio2
#define __ieee754_remainder osc_fdlibm_ieee754_remainder
#define __ieee754_scalb osc_fdlibm_ieee754_scalb
#define __ieee754_sinh osc_fdlibm_ieee754_sinh
#define __ieee754_sqrt osc_fdlibm_ieee754_sqrt
#define __ieee754_y0 osc_fdlibm_ieee754_y0
#define __ieee754_y1 osc_fdlibm_ieee754_y1
#define __ieee754_yn osc_fdlibm_ieee754_yn
#define __kernel_cos osc_fdlibm_kernel_cos
#define __kernel_rem_pio2 osc_fdlibm_kernel_rem_pio2
#define __kernel_sin osc_fdlibm_kernel_sin
#define __kernel_standard osc_fdlibm_kernel_standard
#define __kernel_tan osc_fdlibm_kernel_tan
#endif
