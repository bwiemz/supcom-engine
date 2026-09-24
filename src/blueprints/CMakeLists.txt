add_library(osc_blueprints STATIC
    blueprint_store.cpp
    blueprint_loader.cpp
)
add_library(osc::blueprints ALIAS osc_blueprints)

target_include_directories(osc_blueprints PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
# Blueprints hold Lua registry refs through the raw Lua C API only: the
# library sits below the sim and the bindings, which link it.
target_link_libraries(osc_blueprints
    PUBLIC osc::core lua50
    PRIVATE spdlog::spdlog
)

target_link_libraries(osc_blueprints PRIVATE osc::warnings)
