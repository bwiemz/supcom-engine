# OS abstraction layer: the only place allowed to include platform headers
# (<windows.h>, <unistd.h>, ...). Depends on nothing else in the engine.
add_library(osc_platform STATIC
    crash_handler.cpp
    paths.cpp
)
add_library(osc::platform ALIAS osc_platform)

target_include_directories(osc_platform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(osc_platform PRIVATE osc::warnings)

if(WIN32)
    target_link_libraries(osc_platform PRIVATE shell32 ole32)
endif()
