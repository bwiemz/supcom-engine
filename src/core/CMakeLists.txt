add_library(osc_core STATIC
    image.cpp
    log.cpp
    test_status.cpp
    preferences.cpp
    lua_copy.cpp
    localization.cpp
    game_state.cpp
    front_end_data.cpp
    dmath.cpp
)
add_library(osc::core ALIAS osc_core)

target_include_directories(osc_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(osc_core
    PUBLIC spdlog::spdlog fmt::fmt lua50 nlohmann_json::nlohmann_json
    PRIVATE osc::fdlibm
)

target_link_libraries(osc_core PRIVATE osc::warnings)

# stb single-header libraries (PNG I/O for screenshots / golden images)
target_include_directories(osc_core PRIVATE ${CMAKE_SOURCE_DIR}/third_party)
