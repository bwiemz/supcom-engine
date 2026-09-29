#pragma once

// Which build this is (M227a): the release version, and the source revision
// it was built from.

namespace osc::core {

/// The release version, CMake's project(VERSION): "0.1.0".
const char* version();

/// The source revision: `git describe --always --dirty` when this build was
/// made (a tag, or a commit), "unknown" outside a git checkout.
const char* revision();

/// "<version>-<revision>": what replays and saved games carry, so that a
/// build reads only its own saves.
const char* build_id();

/// "OpenSupCom <version> (<revision>)", as --version prints it.
const char* version_line();

} // namespace osc::core
