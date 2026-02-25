add_library(osc_vfs STATIC
    virtual_file_system.cpp
    directory_mount.cpp
    zip_mount.cpp
)
add_library(osc::vfs ALIAS osc_vfs)

target_include_directories(osc_vfs PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(osc_vfs
    PUBLIC osc::core
    PRIVATE unofficial::minizip::minizip ZLIB::ZLIB
)
