add_library(osc_lua STATIC
    lua_state.cpp
    init_loader.cpp
    engine_bindings.cpp
    blueprint_bindings.cpp
    moho_bindings.cpp
    sim_bindings.cpp
    sim_loader.cpp
    scenario_loader.cpp
    session_manager.cpp
    category_utils.cpp
)
add_library(osc::lua ALIAS osc_lua)

target_include_directories(osc_lua PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(osc_lua
    PUBLIC osc::core osc::vfs osc::sim osc::map lua50
)
