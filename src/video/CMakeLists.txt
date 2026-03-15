add_library(osc_video STATIC
    video_decoder.cpp
)

target_include_directories(osc_video PUBLIC
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/third_party
)

target_link_libraries(osc_video PUBLIC osc_core)

add_library(osc::video ALIAS osc_video)
