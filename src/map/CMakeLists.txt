add_library(osc_map STATIC
    heightmap.cpp
    scmap_parser.cpp
    terrain.cpp
)
add_library(osc::map ALIAS osc_map)

target_include_directories(osc_map PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(osc_map
    PUBLIC osc::core
)
