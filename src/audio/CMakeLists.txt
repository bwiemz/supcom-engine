add_library(osc_audio STATIC
    xwb_parser.cpp
    xsb_parser.cpp
    sound_manager.cpp
)
add_library(osc::audio ALIAS osc_audio)

target_include_directories(osc_audio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_include_directories(osc_audio PRIVATE ${CMAKE_SOURCE_DIR}/third_party/miniaudio)
target_link_libraries(osc_audio
    PUBLIC osc::core
    PRIVATE spdlog::spdlog
)
