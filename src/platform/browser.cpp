#include "platform/browser.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#endif

namespace osc::platform {

#ifdef _WIN32

bool open_url(const std::string& url) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, nullptr, 0);
    if (n <= 0) return false;
    std::wstring wide(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, wide.data(), n);
    // ShellExecuteW reports success as a value above 32.
    const auto result = reinterpret_cast<INT_PTR>(
        ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    return result > 32;
}

#else

extern "C" char** environ; // NOLINT(readability-redundant-declaration): POSIX leaves it undeclared

bool open_url(const std::string& url) {
#ifdef __APPLE__
    const char* handler = "open";
#else
    const char* handler = "xdg-open";
#endif
    std::string program = handler;
    std::string arg = url;
    char* argv[] = {program.data(), arg.data(), nullptr};
    pid_t pid = 0;
    if (posix_spawnp(&pid, handler, nullptr, nullptr, argv, environ) != 0) return false;
    // Reap it when it exits (xdg-open hands the URL on and returns), so it
    // leaves no zombie behind.
    std::thread([pid] { waitpid(pid, nullptr, 0); }).detach();
    return true;
}

#endif

} // namespace osc::platform
