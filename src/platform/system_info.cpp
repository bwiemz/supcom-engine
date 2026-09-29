#include "platform/system_info.hpp"

#include <string>
#include <string_view>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#include <sys/utsname.h>

#include <fstream>
#endif

namespace osc::platform {

#ifdef _WIN32

std::string os_description() {
    // RtlGetVersion tells the truth; GetVersionEx answers what the manifest
    // claims to support
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto get = ntdll ? reinterpret_cast<RtlGetVersionFn>(
                                 reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")))
                           : nullptr;
    std::string text = "Windows";
    if (get && get(&info) == 0)
        text += " " + std::to_string(info.dwMajorVersion) + "." +
                std::to_string(info.dwMinorVersion) + "." + std::to_string(info.dwBuildNumber);
    SYSTEM_INFO sys{};
    GetNativeSystemInfo(&sys);
    text += sys.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64   ? " x64"
            : sys.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64 ? " arm64"
                                                                         : "";
    return text;
}

unsigned long long peak_memory_bytes() {
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    if (!K32GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) return 0;
    return counters.PeakWorkingSetSize;
}

#else

std::string os_description() {
    std::string text;
    utsname name{};
    if (uname(&name) == 0)
        text = std::string(name.sysname) + " " + name.release + " " + name.machine;
    else text = "unknown";
    // The distribution, from os-release's PRETTY_NAME="..."
    std::ifstream release("/etc/os-release");
    for (std::string line; std::getline(release, line);) {
        constexpr std::string_view key = "PRETTY_NAME=";
        if (line.compare(0, key.size(), key) != 0) continue;
        std::string value = line.substr(key.size());
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
            value = value.substr(1, value.size() - 2);
        if (!value.empty()) text += " (" + value + ")";
        break;
    }
    return text;
}

unsigned long long peak_memory_bytes() {
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) return 0;
    // Linux gives kilobytes (macOS would give bytes)
    return static_cast<unsigned long long>(usage.ru_maxrss) * 1024ULL;
}

#endif

} // namespace osc::platform
