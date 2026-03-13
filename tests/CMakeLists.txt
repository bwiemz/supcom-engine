add_executable(osc_tests
    test_vfs.cpp
    test_lua_state.cpp
    test_init_loader.cpp
    test_blueprint_loading.cpp
    test_map.cpp
    test_anim_blend.cpp
    test_dds_decode.cpp
    test_normal_overlay.cpp
    test_frustum.cpp
    test_lod.cpp
    test_death_anim.cpp
    test_preferences.cpp
)

target_link_libraries(osc_tests PRIVATE
    Catch2::Catch2WithMain
    osc::core
    osc::vfs
    osc::map
    osc::lua
    osc::blueprints
    osc::sim
    osc::renderer
)

include(Catch)
catch_discover_tests(osc_tests)
