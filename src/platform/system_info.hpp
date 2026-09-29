#pragma once

#include <string>

namespace osc::platform {

/// The operating system, for a bug report (M228b): e.g. "Linux 6.9.1
/// x86_64 (Ubuntu 22.04.4 LTS)" or "Windows 10.0.22631 x64".
std::string os_description();

} // namespace osc::platform
