#pragma once

#include <string>

namespace osc::platform {

/// The operating system, for a bug report (M228b): e.g. "Linux 6.9.1
/// x86_64 (Ubuntu 22.04.4 LTS)" or "Windows 10.0.22631 x64".
std::string os_description();

/// The most memory this process has held at once (its peak resident set),
/// in bytes; 0 if the system won't say.
unsigned long long peak_memory_bytes();

} // namespace osc::platform
