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
#include <unistd.h>

#include <array>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <system_error>
#endif
#ifdef __APPLE__
#include <mach/mach.h>
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

MemoryUsage memory_usage() {
    MemoryUsage usage;
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
        usage.resident = counters.WorkingSetSize;
    // The process's address space in use: all of it less what is free
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status))
        usage.address_space = status.ullTotalVirtual - status.ullAvailVirtual;
    return usage;
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
#ifdef __APPLE__
    // macOS gives bytes (its getrusage(2) man page)
    return static_cast<unsigned long long>(usage.ru_maxrss);
#else
    // Linux gives kilobytes
    return static_cast<unsigned long long>(usage.ru_maxrss) * 1024ULL;
#endif
}

MemoryUsage memory_usage() {
    MemoryUsage usage;
#ifdef __APPLE__
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info),
                  &count) == KERN_SUCCESS) {
        usage.address_space = info.virtual_size;
        usage.resident = info.resident_size;
    }
#else
    // statm's first two fields: the address space and the resident set, in
    // pages
    if (std::FILE* statm = std::fopen("/proc/self/statm", "r")) {
        std::array<char, 128> line{};
        const bool read = std::fgets(line.data(), static_cast<int>(line.size()), statm) != nullptr;
        std::fclose(statm);
        unsigned long long size = 0;
        unsigned long long resident = 0;
        const char* end = line.data() + std::strlen(line.data());
        const auto first = std::from_chars(line.data(), end, size);
        const auto second = first.ec == std::errc{} && first.ptr < end
                                ? std::from_chars(first.ptr + 1, end, resident)
                                : std::from_chars_result{end, std::errc::invalid_argument};
        if (read && second.ec == std::errc{}) {
            const long page = sysconf(_SC_PAGESIZE);
            const unsigned long long bytes = page > 0 ? static_cast<unsigned long long>(page) : 0;
            usage.address_space = size * bytes;
            usage.resident = resident * bytes;
        }
    }
#endif
    return usage;
}

#endif

} // namespace osc::platform
