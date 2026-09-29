#pragma once

namespace osc::sim {

/// This build's identity ("<version>-<revision>", core::build_id), as
/// replays and saved games record it.
const char* build_id();

} // namespace osc::sim
