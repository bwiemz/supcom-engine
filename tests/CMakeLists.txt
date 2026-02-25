add_executable(osc_tests
    test_vfs.cpp
    test_lua_state.cpp
    test_init_loader.cpp
    test_blueprint_loading.cpp
    test_map.cpp
)

target_link_libraries(osc_tests PRIVATE
    Catch2::Catch2WithMain
    osc::core
    osc::vfs
    osc::map
    osc::lua
    osc::blueprints
)

include(Catch)
catch_discover_tests(osc_tests)
