# vcpkg's arm64-osx triplet, with the dependencies built for the game's
# deployment target (CMakePresets.json's macos-* presets). Without it vcpkg
# builds them for the host's macOS, and every object linked into the game
# warns "built for newer 'macOS' version than being linked" -- and a game
# built on a new macOS might not start on an older one.
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES arm64)
set(VCPKG_OSX_DEPLOYMENT_TARGET 14.0)
