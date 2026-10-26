#pragma once

#include <string>

namespace osc::platform {

/// The operating system, for a bug report (M228b): e.g. "Linux 6.9.1
/// x86_64 (Ubuntu 22.04.4 LTS)" or "Windows 10.0.22631 x64".
std::string os_description();

/// The most memory this process has held at once (its peak resident set),
/// in bytes; 0 if the system won't say.
unsigned long long peak_memory_bytes();

/// The memory this process holds now, in bytes (0s if the system won't say):
/// its address space, and what of it is resident. Cheap enough for every
/// frame.
struct MemoryUsage {
    unsigned long long address_space = 0;
    unsigned long long resident = 0;
};
MemoryUsage memory_usage();

} // namespace osc::platform
