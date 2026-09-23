# FDLIBM 5.3 (Sun Microsystems), as Android ships it -- the library Java's
# StrictMath is built on, whose results are specified to be the same on every
# platform. OpenSupCom vendors the subset its sim needs so that sin, atan2,
# pow and the rest give the same bits everywhere (the platforms' own libms
# differ in the last bit). See README.osc.
add_library(osc_fdlibm STATIC
    e_acos.c e_asin.c e_atan2.c e_exp.c e_hypot.c e_log.c e_log10.c e_pow.c
    e_rem_pio2.c e_sqrt.c
    k_cos.c k_rem_pio2.c k_sin.c k_tan.c
    s_atan.c s_copysign.c s_cos.c s_fabs.c s_finite.c s_floor.c s_scalbn.c
    s_sin.c s_tan.c
    w_acos.c w_asin.c w_atan2.c w_exp.c w_hypot.c w_log.c w_log10.c w_pow.c
    w_sqrt.c
)
add_library(osc::fdlibm ALIAS osc_fdlibm)

# C11 without extensions: no contraction, and MSVC then defines __STDC__
# (with /Zc:__STDC__), so FDLIBM uses prototypes rather than K&R forms.
set_target_properties(osc_fdlibm PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_include_directories(osc_fdlibm PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# As Android builds it: IEEE-only behaviour (no SVID error handling), and a
# little-endian word order (x86-64 and ARM64 both are).
target_compile_definitions(osc_fdlibm PRIVATE _IEEE_LIBM __LITTLE_ENDIAN)
if(MSVC)
    target_compile_options(osc_fdlibm PRIVATE /W0 /Zc:__STDC__)
else()
    # FDLIBM reads a double's words through int pointers.
    target_compile_options(osc_fdlibm PRIVATE -w -fno-strict-aliasing)
endif()
