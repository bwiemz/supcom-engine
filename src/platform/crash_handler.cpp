#include "platform/crash_handler.hpp"

#include <atomic>
#include <cstdint>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#else
#include <csignal>
#include <cstring>
#include <execinfo.h>
#include <unistd.h>
#endif

namespace osc::platform {

namespace {

std::atomic<CrashFlushHook> g_flush{nullptr};

} // namespace

#ifdef _WIN32

namespace {

LONG WINAPI unhandled_exception_filter(EXCEPTION_POINTERS* ep) {
    std::fprintf(stderr,
                 "\n*** OpenSupCom crashed: exception 0x%08lx at %p ***\n",
                 static_cast<unsigned long>(ep->ExceptionRecord->ExceptionCode),
                 ep->ExceptionRecord->ExceptionAddress);
    std::fflush(stderr);
    if (auto flush = g_flush.load()) flush();
    return EXCEPTION_EXECUTE_HANDLER;
}

} // namespace

void install_crash_handler(CrashFlushHook flush) {
    g_flush.store(flush);
    SetUnhandledExceptionFilter(unhandled_exception_filter);
}

#else

namespace {

// Everything below runs inside a signal handler: only async-signal-safe
// calls (write, backtrace_symbols_fd, signal, raise), no allocation.

void write_str(const char* s) {
    ssize_t ignored = write(STDERR_FILENO, s, std::strlen(s));
    static_cast<void>(ignored);
}

void write_hex(std::uintptr_t value) {
    char buf[2 + sizeof(value) * 2 + 1];
    char* p = buf + sizeof(buf) - 1;
    *p = '\0';
    do {
        *--p = "0123456789abcdef"[value & 0xF];
        value >>= 4;
    } while (value != 0);
    *--p = 'x';
    *--p = '0';
    write_str(p);
}

const char* signal_name(int sig) {
    switch (sig) {
    case SIGSEGV: return "SIGSEGV (invalid memory access)";
    case SIGBUS: return "SIGBUS (bus error)";
    case SIGFPE: return "SIGFPE (arithmetic error)";
    case SIGILL: return "SIGILL (illegal instruction)";
    case SIGABRT: return "SIGABRT (abort)";
    default: return "fatal signal";
    }
}

constexpr int kFatalSignals[] = {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT};

std::atomic<bool> g_in_handler{false};

void fatal_signal_handler(int sig, siginfo_t* info, void* /*context*/) {
    // A second fault while reporting (or a fault on another thread) goes
    // straight to the default action.
    if (g_in_handler.exchange(true)) {
        signal(sig, SIG_DFL);
        raise(sig);
        return;
    }

    write_str("\n*** OpenSupCom crashed: ");
    write_str(signal_name(sig));
    if (info && (sig == SIGSEGV || sig == SIGBUS)) {
        write_str(", fault address ");
        write_hex(reinterpret_cast<std::uintptr_t>(info->si_addr));
    }
    write_str(" ***\nBacktrace (resolve offsets with addr2line -e <binary>):\n");

    void* frames[64];
    int count = backtrace(frames, 64);
    backtrace_symbols_fd(frames, count, STDERR_FILENO);

    if (auto flush = g_flush.load()) flush();

    // SA_RESETHAND already restored the default action; re-raise so the
    // process still dies by this signal (core dump, correct exit status).
    signal(sig, SIG_DFL);
    raise(sig);
}

} // namespace

void install_crash_handler(CrashFlushHook flush) {
    g_flush.store(flush);

    static std::atomic<bool> installed{false};
    if (installed.exchange(true)) return;

    // A stack overflow leaves no stack to run the handler on.
    static char alt_stack[64 * 1024];
    stack_t ss{};
    ss.ss_sp = alt_stack;
    ss.ss_size = sizeof(alt_stack);
    sigaltstack(&ss, nullptr);

    // backtrace() may allocate the first time it runs (it loads the unwinder);
    // do that now rather than inside the handler.
    void* warm[1];
    backtrace(warm, 1);

    struct sigaction sa{};
    sa.sa_sigaction = fatal_signal_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    for (int sig : kFatalSignals) {
        sigaction(sig, &sa, nullptr);
    }

    signal(SIGPIPE, SIG_IGN);
}

#endif

} // namespace osc::platform
