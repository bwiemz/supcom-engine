add_library(osc_sim STATIC
    anim_cache.cpp
    armor_definition.cpp
    blueprint_categories.cpp
    category_expr.cpp
    build_placement.cpp
    influence_map.cpp
    army_brain.cpp
    bone_cache.cpp
    bone_data.cpp
    collision.cpp
    collision_beam.cpp
    entity.cpp
    formation.cpp
    manipulator.cpp
    platoon.cpp
    sca_parser.cpp
    scm_parser.cpp
    unit.cpp
    unit_orders.cpp
    unit_staging.cpp
    unit_storage.cpp
    weapon.cpp
    projectile.cpp
    shield.cpp
    navigator.cpp
    entity_registry.cpp
    thread_manager.cpp
    transport_slots.cpp
    replay.cpp
    category_set.cpp
    saved_game.cpp
    lockstep_session.cpp
    net_transport.cpp
    sim_state.cpp
    work_range.cpp
    world_snapshot.cpp
    command_codec.cpp
    game_setup.cpp
    script_class.cpp
    projectile_script.cpp
    prop_script.cpp
    build_info.cpp
    sim_callbacks.cpp
)
add_library(osc::sim ALIAS osc_sim)

target_include_directories(osc_sim PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(osc_sim
    PUBLIC osc::core osc::map osc::blueprints lua50
    PRIVATE spdlog::spdlog osc::audio osc::vfs
)

# Winsock for the TCP transport (POSIX sockets need no extra link).
if(WIN32)
    target_link_libraries(osc_sim PRIVATE ws2_32)
endif()

target_link_libraries(osc_sim PRIVATE osc::warnings)

# The build's identity, stamped into replays: informational (playback warns
# when a replay came from another build). Taken at configure time, on the one
# file that uses it, so a reconfigure rebuilds only that.
execute_process(
    COMMAND git describe --always --dirty
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE OSC_GIT_DESCRIBE
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT OSC_GIT_DESCRIBE)
    set(OSC_GIT_DESCRIBE "unknown")
endif()
set_source_files_properties(build_info.cpp PROPERTIES
    COMPILE_DEFINITIONS "OSC_BUILD_ID=\"${PROJECT_VERSION}-${OSC_GIT_DESCRIBE}\"")
