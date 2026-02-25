add_library(osc_blueprints STATIC
    blueprint_store.cpp
    blueprint_loader.cpp
)
add_library(osc::blueprints ALIAS osc_blueprints)

target_include_directories(osc_blueprints PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(osc_blueprints
    PUBLIC osc::core osc::lua
)
