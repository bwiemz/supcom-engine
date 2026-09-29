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
    color.cpp
    version.cpp
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

# Which build this is (core/version, M227a). The version is CMake's; the
# source revision is taken at every build, since a configure-time one goes
# stale as commits land (and replays and saves compare build ids). Written at
# configure time too, so the header exists for tools reading
# compile_commands.json before a build.
set(OSC_REVISION_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
set(OSC_REVISION_ARGS -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
                      -DOUTPUT=${OSC_REVISION_DIR}/osc_revision.hpp
                      -P ${CMAKE_SOURCE_DIR}/cmake/OscRevision.cmake)
execute_process(COMMAND ${CMAKE_COMMAND} ${OSC_REVISION_ARGS})
add_custom_target(osc_revision
    COMMAND ${CMAKE_COMMAND} ${OSC_REVISION_ARGS}
    BYPRODUCTS ${OSC_REVISION_DIR}/osc_revision.hpp
    VERBATIM)
add_dependencies(osc_core osc_revision)
target_include_directories(osc_core PRIVATE ${OSC_REVISION_DIR})
set_source_files_properties(version.cpp PROPERTIES
    COMPILE_DEFINITIONS "OSC_VERSION=\"${PROJECT_VERSION}\"")
