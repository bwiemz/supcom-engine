add_library(osc_lua STATIC
    lua_state.cpp
    init_loader.cpp
    engine_bindings.cpp
    blueprint_bindings.cpp
    moho_bindings.cpp
    mp_net_state.cpp
    lan_lobby.cpp
    sim_bindings.cpp
    sim_loader.cpp
    sim_sync.cpp
    script_loader.cpp
    scenario_loader.cpp
    session_manager.cpp
    special_files.cpp
    category_utils.cpp
    factory_queue.cpp
    order_helpers.cpp
    beat_system.cpp
    smoke_test.cpp
    binding_coverage.cpp
)
add_library(osc::lua ALIAS osc_lua)

target_include_directories(osc_lua PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(osc_lua
    PUBLIC osc::core osc::vfs osc::sim osc::map osc::audio osc::ui osc::video lua50
           osc::platform osc::blueprints
)

target_link_libraries(osc_lua PRIVATE osc::warnings)

# The UI state's bindings that reach the game's view and input (M191): apart
# from osc_lua, so the sim's Lua library needs no renderer.
add_library(osc_lua_user STATIC
    user_bindings.cpp
)
add_library(osc::lua_user ALIAS osc_lua_user)
target_link_libraries(osc_lua_user
    PUBLIC osc::lua
    PRIVATE osc::renderer spdlog::spdlog osc::warnings
)
