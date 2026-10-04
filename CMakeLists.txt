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

include(cmake/OscCompileOptions.cmake)

# Vendored Lua 5.0
add_subdirectory(third_party/fdlibm)
add_subdirectory(third_party/lua-5.0)

# Engine libraries
add_subdirectory(src/platform)
add_subdirectory(src/core)
add_subdirectory(src/vfs)
add_subdirectory(src/map)
add_subdirectory(src/sim)
add_subdirectory(src/audio)
add_subdirectory(src/video)
add_subdirectory(src/ui)
add_subdirectory(src/renderer)
add_subdirectory(src/lua)
add_subdirectory(src/blueprints)
add_subdirectory(src/app)

# The game. Its test modes are in the integration runner, osc_integration
# (tests/integration/runner).
add_executable(opensupcom src/main.cpp)
target_link_libraries(opensupcom PRIVATE osc::warnings osc::app)

# Export symbols from the executable so crash backtraces show function names.
set_target_properties(opensupcom PROPERTIES ENABLE_EXPORTS ON)

# A self-contained game for the Linux packages (M227b): the C++ runtime
# linked in, so a player's system needs only glibc and the Vulkan loader.
# (Windows ships the Visual C++ runtime's DLLs beside the game instead.)
option(OSC_STATIC_RUNTIME "Link the C++ runtime into the game (packages)" OFF)
if(OSC_STATIC_RUNTIME AND NOT MSVC)
    target_link_options(opensupcom PRIVATE -static-libstdc++ -static-libgcc)
endif()

# Installation (M227a): the game and its documents; on Linux, its desktop
# entry and icon too. The test runners and tools stay in the build tree.
# Windows lays the game out flat, as its zip carries it, and vcpkg installs
# the DLLs it links beside it.
include(GNUInstallDirs)
if(WIN32)
    set(OSC_INSTALL_BINDIR .)
    set(OSC_INSTALL_DOCDIR .)
else()
    set(OSC_INSTALL_BINDIR ${CMAKE_INSTALL_BINDIR})
    set(OSC_INSTALL_DOCDIR ${CMAKE_INSTALL_DATADIR}/doc/opensupcom)
endif()
# (ENABLE_EXPORTS gives the game an import library on Windows: not shipped)
if(APPLE)
    # vcpkg's dylibs (the Vulkan loader) go into lib/, where the installed game
    # looks for them by @rpath
    set(OSC_RUNTIME_DEPENDENCIES RUNTIME_DEPENDENCY_SET opensupcom_dylibs)
    set_target_properties(opensupcom PROPERTIES
        INSTALL_RPATH "@loader_path/../${CMAKE_INSTALL_LIBDIR}")
endif()
install(TARGETS opensupcom ${OSC_RUNTIME_DEPENDENCIES}
                RUNTIME DESTINATION ${OSC_INSTALL_BINDIR}
                ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} EXCLUDE_FROM_ALL)
if(APPLE)
    install(RUNTIME_DEPENDENCY_SET opensupcom_dylibs
            PRE_EXCLUDE_REGEXES "^/usr/lib/" "^/System/"
            POST_EXCLUDE_REGEXES "^/usr/lib/" "^/System/"
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()
if(COMMAND x_vcpkg_install_local_dependencies) # the vcpkg toolchain's; Windows only
    x_vcpkg_install_local_dependencies(TARGETS opensupcom DESTINATION ${OSC_INSTALL_BINDIR})
endif()
install(FILES README.md LICENSE CHANGELOG.md DESTINATION ${OSC_INSTALL_DOCDIR})
if(UNIX AND NOT APPLE)
    install(FILES packaging/linux/opensupcom.desktop
            DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES packaging/opensupcom.svg
            DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
endif()
if(MSVC)
    # The Visual C++ runtime beside the game: no redistributable to install
    # (Windows 10 and later carry the universal CRT)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION ${OSC_INSTALL_BINDIR})
    include(InstallRequiredSystemLibraries)
endif()

# Packages (M227b): `cpack` zips the install (Windows) or tars it (Linux);
# tools/package/appimage.sh makes the AppImage, and .github/workflows/
# release.yml builds and runs all of them.
set(CPACK_PACKAGE_NAME OpenSupCom)
set(CPACK_PACKAGE_VENDOR OpenSupCom)
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Open-source engine for Supreme Commander: Forged Alliance")
set(CPACK_RESOURCE_FILE_LICENSE ${CMAKE_SOURCE_DIR}/LICENSE)
if(WIN32)
    set(CPACK_GENERATOR ZIP)
    set(OSC_PACKAGE_SYSTEM win64)
else()
    set(CPACK_GENERATOR TGZ)
    string(TOLOWER "${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}" OSC_PACKAGE_SYSTEM)
endif()
set(CPACK_PACKAGE_FILE_NAME OpenSupCom-${PROJECT_VERSION}-${OSC_PACKAGE_SYSTEM})
include(CPack)

# Tests
option(OSC_BUILD_TESTS "Build unit tests" ON)
if(OSC_BUILD_TESTS)
    find_package(Catch2 3 CONFIG REQUIRED)
    enable_testing()
    add_subdirectory(tests)
    add_subdirectory(tests/integration)

    # The library targets link without a cycle or a layer crossing (M191):
    # the sim and its Lua library never reach the renderer.
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        include(cmake/OscLinkGraph.cmake)
        osc_write_link_graph(${CMAKE_BINARY_DIR}/osc_link_graph.txt)
        add_test(NAME arch.link_layers
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/check_link_cycles.py
                         ${CMAKE_BINARY_DIR}/osc_link_graph.txt)
        set_tests_properties(arch.link_layers PROPERTIES LABELS "arch" TIMEOUT 60)

        # No string literal outgrows MSVC's caps (16,380 bytes a literal,
        # C2026): GCC and Clang take one, so a shader grown on another
        # platform breaks only the Windows build.
        add_test(NAME arch.literal_size
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/check_literal_size.py
                         ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/tests)
        add_test(NAME arch.literal_size_self_test
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/check_literal_size.py
                         --self-test)
        set_tests_properties(arch.literal_size arch.literal_size_self_test
                             PROPERTIES LABELS "arch" TIMEOUT 60)

        # Every field of the sim's classes is in a snapshot serializer, or
        # named there with why it isn't (M208c): a field forgotten is a
        # silent desync after a load.
        add_test(NAME arch.state_io
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/check_state_io.py
                         ${CMAKE_SOURCE_DIR})
        add_test(NAME arch.state_io_self_test
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/check_state_io.py
                         --self-test)
        set_tests_properties(arch.state_io arch.state_io_self_test
                             PROPERTIES LABELS "arch" TIMEOUT 60)

        # Every Lua binding that does nothing is listed as a stub: one added
        # unlisted passes the coverage ratchet and quietly drops what a script
        # asks for.
        add_test(NAME arch.lua_stubs
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/check_lua_stubs.py
                         ${CMAKE_SOURCE_DIR})
        add_test(NAME arch.lua_stubs_self_test
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/check_lua_stubs.py
                         --self-test)
        set_tests_properties(arch.lua_stubs arch.lua_stubs_self_test
                             PROPERTIES LABELS "arch" TIMEOUT 60)

        # retail_ref.py, which asks retail FA itself for references: its config,
        # init file and reading of retail's log (retail isn't run here)
        add_test(NAME arch.retail_ref_self_test
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/retail_ref/retail_ref.py
                         --self-test)
        set_tests_properties(arch.retail_ref_self_test PROPERTIES LABELS "arch" TIMEOUT 60)

        # The docs' status numbers are this build's (tools/status_metrics.py):
        # a change that adds tests updates docs/current-state.md with them.
        # Not on Windows, whose build leaves out some POSIX-only tests.
        if(UNIX)
            add_test(NAME arch.status_metrics
                     COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/status_metrics.py
                             check --build-dir ${CMAKE_BINARY_DIR})
            add_test(NAME arch.status_metrics_self_test
                     COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/status_metrics.py
                             --self-test)
            set_tests_properties(arch.status_metrics arch.status_metrics_self_test
                                 PROPERTIES LABELS "arch" TIMEOUT 120)
        endif()

        # The version is written in CMake, vcpkg.json and CHANGELOG.md, and
        # they agree; an install of this build runs on its own (M227a).
        add_test(NAME arch.version
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/check_version.py
                         ${CMAKE_SOURCE_DIR})
        add_test(NAME arch.version_self_test
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/check_version.py
                         --self-test)
        add_test(NAME arch.install
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/check_install.py
                         ${CMAKE_BINARY_DIR} $<CONFIG> ${PROJECT_VERSION})
        add_test(NAME arch.bench_self_test
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/bench.py --self-test)
        set_tests_properties(arch.version arch.version_self_test arch.install
                             arch.bench_self_test PROPERTIES LABELS "arch" TIMEOUT 120)
    endif()
endif()

# Static analysis (label `lint`, off by default: it needs clang-tidy, the
# version the baseline was taken with). `ctest -L lint` runs the ratchet in
# tools/clang_tidy_ratchet.py over this build's compile_commands.json.
option(OSC_LINT "Register the clang-tidy ratchet as a test (label lint)" OFF)
if(OSC_LINT AND OSC_BUILD_TESTS)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME lint.clang_tidy
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/clang_tidy_ratchet.py
                     -p ${CMAKE_BINARY_DIR}
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    set_tests_properties(lint.clang_tidy PROPERTIES LABELS "lint" TIMEOUT 1800)
endif()
