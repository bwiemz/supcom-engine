add_library(osc_sim STATIC
    army_brain.cpp
    bone_cache.cpp
    bone_data.cpp
    entity.cpp
    manipulator.cpp
    platoon.cpp
    scm_parser.cpp
    unit.cpp
    weapon.cpp
    projectile.cpp
    shield.cpp
    navigator.cpp
    entity_registry.cpp
    thread_manager.cpp
    sim_state.cpp
)
add_library(osc::sim ALIAS osc_sim)

target_include_directories(osc_sim PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(osc_sim
    PUBLIC osc::core osc::map lua50
    PRIVATE spdlog::spdlog osc::audio osc::vfs
)
