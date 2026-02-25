add_library(osc_core STATIC
    log.cpp
)
add_library(osc::core ALIAS osc_core)

target_include_directories(osc_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(osc_core
    PUBLIC spdlog::spdlog fmt::fmt lua50
)
