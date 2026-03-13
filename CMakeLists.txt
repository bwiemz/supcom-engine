cmake_minimum_required(VERSION 3.21)
project(OpenSupCom VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# vcpkg dependencies
find_package(ZLIB REQUIRED)
find_package(unofficial-minizip CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)
find_package(Vulkan REQUIRED)
find_package(glfw3 CONFIG REQUIRED)
find_package(VulkanMemoryAllocator CONFIG REQUIRED)
find_package(vk-bootstrap CONFIG REQUIRED)
find_package(unofficial-shaderc CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

# Vendored Lua 5.0
add_subdirectory(third_party/lua-5.0)

# Engine libraries
add_subdirectory(src/core)
add_subdirectory(src/vfs)
add_subdirectory(src/map)
add_subdirectory(src/sim)
add_subdirectory(src/audio)
add_subdirectory(src/ui)
add_subdirectory(src/renderer)
add_subdirectory(src/lua)
add_subdirectory(src/blueprints)

# Main executable
add_executable(opensupcom src/main.cpp src/integration_tests.cpp)
target_link_libraries(opensupcom PRIVATE
    osc::core
    osc::vfs
    osc::sim
    osc::map
    osc::audio
    osc::ui
    osc::renderer
    osc::lua
    osc::blueprints
)

# Tests
option(OSC_BUILD_TESTS "Build unit tests" ON)
if(OSC_BUILD_TESTS)
    find_package(Catch2 3 CONFIG REQUIRED)
    enable_testing()
    add_subdirectory(tests)
endif()
