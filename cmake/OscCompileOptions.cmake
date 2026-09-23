# Project-wide compile options.
#
# Warnings apply only to first-party targets (link osc::warnings PRIVATE);
# vendored code keeps its own settings. Sanitizers apply to every target so
# ASan/UBSan see the whole process, vendored Lua included.

add_library(osc_warnings INTERFACE)
add_library(osc::warnings ALIAS osc_warnings)
if(MSVC)
    target_compile_options(osc_warnings INTERFACE /W3 /permissive- /utf-8)
else()
    target_compile_options(osc_warnings INTERFACE -Wall -Wextra -Wno-unused-parameter)
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
