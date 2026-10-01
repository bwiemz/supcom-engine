#pragma once

// The running executable itself: its file, and the build id its linker gave
// it. A game snapshot names C functions by their place in the binary, so
// only the binary that took it may restore it (the saved game's build id is
// shared by every dirty build of a commit, and by its Debug and Release).

#include <cstdint>
#include <filesystem>
#include <vector>

namespace osc::platform {

/// The running executable's file; empty if the OS won't say.
std::filesystem::path executable_path();

/// The build id the linker wrote into the running executable (ELF's
/// NT_GNU_BUILD_ID note, a digest of its contents), or empty where it has
/// none or the platform keeps none this reads.
std::vector<std::uint8_t> executable_build_id();

} // namespace osc::platform
