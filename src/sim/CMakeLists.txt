add_library(osc_sim STATIC
    anim_cache.cpp
    armor_definition.cpp
    blueprint_categories.cpp
    category_expr.cpp
    build_placement.cpp
    influence_map.cpp
    army_brain.cpp
    game_colors.cpp
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
    steering.cpp
    entity_registry.cpp
    thread_manager.cpp
    transport_slots.cpp
    replay.cpp
    category_set.cpp
    saved_game.cpp
    lockstep_session.cpp
    net_transport.cpp
    lobby_net.cpp
    lan_discovery.cpp
    sim_state.cpp
    work_range.cpp
    decal.cpp
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
