# Project-wide compile options.
#
# Warnings apply only to first-party targets (link osc::warnings PRIVATE);
# vendored code keeps its own settings. Sanitizers apply to every target so
# ASan/UBSan see the whole process, vendored Lua included.

# MSVC: /EHs instead of the default /EHsc. Lua is built as C++ and raises
# errors as C++ exceptions through its extern "C" API; /EHsc lets the
# compiler assume extern "C" functions never throw and drop the unwind
# (destructor) code around every Lua call.
if(MSVC)
    string(REPLACE "/EHsc" "/EHs" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    if(NOT CMAKE_CXX_FLAGS MATCHES "/EHs")
        string(APPEND CMAKE_CXX_FLAGS " /EHs")
    endif()
endif()

# Floating point: the sim must compute the same bits on every compiler and
# CPU (lockstep, replays). No contraction into fused multiply-adds -- GCC's
# default outside ISO mode and Clang's within an expression; it changes the
# rounding wherever the CPU has FMA (ARM64, x86-64 built for AVX2) -- and
# MSVC's precise model. x86-64 uses SSE2, so there is no x87 excess
# precision; 32-bit x86 would have it.
if(MSVC)
    add_compile_options(/fp:precise)
else()
    add_compile_options(-ffp-contract=off)
endif()
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(WARNING "32-bit builds may use x87 excess precision: the sim would not be "
                    "deterministic across platforms")
endif()

add_library(osc_warnings INTERFACE)
add_library(osc::warnings ALIAS osc_warnings)
if(MSVC)
    target_compile_options(osc_warnings INTERFACE /W3 /permissive- /utf-8)
else()
    target_compile_options(osc_warnings INTERFACE -Wall -Wextra -Wno-unused-parameter)
endif()

# Warnings as errors for first-party code on GCC/Clang. Off by default, so a
# newer local compiler's new warnings don't stop a build; CI's Linux jobs
# turn it on, so the code stays warning-clean on the compilers CI pins.
option(OSC_WERROR "Treat first-party warnings as errors (GCC/Clang)" OFF)
if(OSC_WERROR AND NOT MSVC)
    target_compile_options(osc_warnings INTERFACE -Werror)
endif()

set(OSC_SANITIZE "" CACHE STRING
    "Sanitizers to enable on GCC/Clang, e.g. address;undefined")
if(OSC_SANITIZE)
    if(MSVC)
        message(WARNING "OSC_SANITIZE is only supported with GCC/Clang; ignoring")
    else()
        list(JOIN OSC_SANITIZE "," _osc_sanitizers)
        add_compile_options(-fsanitize=${_osc_sanitizers} -fno-omit-frame-pointer)
        add_link_options(-fsanitize=${_osc_sanitizers})
        message(STATUS "Sanitizers enabled: ${_osc_sanitizers}")
    endif()
endif()
