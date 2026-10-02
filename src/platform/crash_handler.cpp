#include "platform/crash_handler.hpp"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <system_error>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <ctime>
#include <cwchar>
#include <iterator>
#else
#include <csignal>
#include <ctime>
#include <execinfo.h>
#include <fcntl.h>
#include <unistd.h>
#ifdef __linux__
#include <link.h>
#include <ucontext.h>
#endif
#endif

namespace osc::platform {

namespace {

// Kept ready for the handler, which must not allocate (M228b): the report
// folder (with its separator) and the header, as NUL-terminated text.
// Empty: no file.
char g_report_dir[4096];
char g_report_header[1024];
#ifdef _WIN32
// The folder as Windows names it (UTF-16): a narrow copy would lose a
// profile path outside the system code page
wchar_t g_report_dir_w[4096];
#endif

/// Copy `text` into `out` (NUL-terminated); false if it doesn't fit.
bool copy_text(char* out, size_t size, const std::string& text) {
    if (text.size() + 1 > size) return false;
    std::memcpy(out, text.c_str(), text.size() + 1);
    return true;
}

} // namespace

void set_crash_report_dir(const std::filesystem::path& dir, const std::string& header) {
    g_report_dir[0] = '\0';
    g_report_header[0] = '\0';
    if (dir.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
#ifdef _WIN32
    g_report_dir_w[0] = L'\0';
    const std::wstring wide = (dir / "").wstring(); // with its separator
    if (wide.size() + 1 > std::size(g_report_dir_w)) return;
    std::wmemcpy(g_report_dir_w, wide.c_str(), wide.size() + 1);
#else
    const std::string folder = (dir / "").string(); // with its separator
    if (!copy_text(g_report_dir, sizeof(g_report_dir), folder)) return;
#endif
    if (!copy_text(g_report_header, sizeof(g_report_header), header + "\n"))
        copy_text(g_report_header, sizeof(g_report_header), "\n");
}

#ifdef _WIN32

namespace {

/// The report's text, into `out`.
int format_report(char* out, size_t size, EXCEPTION_POINTERS* ep) {
    return std::snprintf(out, size, "\n*** OpenSupCom crashed: exception 0x%08lx at %p ***\n",
                         static_cast<unsigned long>(ep->ExceptionRecord->ExceptionCode),
                         ep->ExceptionRecord->ExceptionAddress);
}

/// The report into the report folder, if there is one; the file's path to
/// `path`.
bool write_report_file(const char* report, wchar_t* path, size_t path_size) {
    if (g_report_dir_w[0] == L'\0') return false;
    std::swprintf(path, path_size, L"%lscrash-%lld-%lu.txt", g_report_dir_w,
                  static_cast<long long>(std::time(nullptr)),
                  static_cast<unsigned long>(GetCurrentProcessId()));
    const HANDLE file =
        CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    WriteFile(file, g_report_header, static_cast<DWORD>(std::strlen(g_report_header)), &written,
              nullptr);
    WriteFile(file, report, static_cast<DWORD>(std::strlen(report)), &written, nullptr);
    CloseHandle(file);
    return true;
}

LONG WINAPI unhandled_exception_filter(EXCEPTION_POINTERS* ep) {
    char report[256];
    format_report(report, sizeof(report), ep);
    std::fputs(report, stderr);
    // (static: a stack overflow leaves the filter little stack)
    static wchar_t path[std::size(g_report_dir_w) + 64];
    if (write_report_file(report, path, std::size(path)))
        std::fprintf(stderr, "Crash report: %ls\n", path);
    std::fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}

} // namespace

void install_crash_handler() {
    SetUnhandledExceptionFilter(unhandled_exception_filter);
}

void set_unwinder_for_test(UnwinderForTest /*unwinder*/) {} // (the filter walks no stack)

void crash_for_test() {
    RaiseException(EXCEPTION_ACCESS_VIOLATION, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    std::abort();
}

#else

namespace {

// Everything below runs inside a signal handler: only async-signal-safe
// calls (open, write, close, clock_gettime, getpid, backtrace_symbols_fd,
// signal, raise), no allocation.

void write_str(int fd, const char* s) {
    ssize_t ignored = write(fd, s, std::strlen(s));
    static_cast<void>(ignored);
}

/// `value` in `base` (10 or 16, with 0x) into `out` (at least 24 bytes).
void format_number(char* out, std::uintmax_t value, unsigned base) {
    char buf[24];
    char* p = buf + sizeof(buf) - 1;
    *p = '\0';
    do {
        *--p = "0123456789abcdef"[value % base];
        value /= base;
    } while (value != 0);
    if (base == 16) {
        *--p = 'x';
        *--p = '0';
    }
    std::memcpy(out, p, static_cast<size_t>(buf + sizeof(buf) - p));
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

/// The unwinder: backtrace(), or a test's.
UnwinderForTest g_unwinder = &backtrace;

/// The main program's code in memory, found when the handler is installed:
/// a PC in it is reported as the address addr2line takes (PC - load bias).
std::uintptr_t g_image_bias = 0;
std::uintptr_t g_image_begin = 0;
std::uintptr_t g_image_end = 0;

#ifdef __linux__
int find_main_image(dl_phdr_info* info, size_t /*size*/, void* /*data*/) {
    // The first object dl_iterate_phdr visits is the main program
    g_image_bias = info->dlpi_addr;
    for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr) & ph = info->dlpi_phdr[i];
        if (ph.p_type != PT_LOAD || (ph.p_flags & PF_X) == 0) continue;
        const std::uintptr_t begin = info->dlpi_addr + ph.p_vaddr;
        const std::uintptr_t end = begin + ph.p_memsz;
        if (g_image_begin == 0 || begin < g_image_begin) g_image_begin = begin;
        if (end > g_image_end) g_image_end = end;
    }
    return 1;
}
#endif

/// Where it was running when the signal came: the faulting instruction for
/// a fault (0 where the platform's context isn't read: Linux on x86-64 and
/// AArch64 only).
std::uintptr_t fault_pc(const void* context) {
#if defined(__linux__) && defined(__x86_64__)
    return context ? static_cast<std::uintptr_t>(
                         static_cast<const ucontext_t*>(context)->uc_mcontext.gregs[REG_RIP])
                   : 0;
#elif defined(__linux__) && defined(__aarch64__)
    return context ? static_cast<std::uintptr_t>(
                         static_cast<const ucontext_t*>(context)->uc_mcontext.pc)
                   : 0;
#else
    static_cast<void>(context);
    return 0;
#endif
}

/// What crashed and where, written before any unwinding: on a smashed stack
/// the unwinder can fault in turn, and this much is then already out.
void write_summary(int fd, int sig, const siginfo_t* info, std::uintptr_t pc) {
    write_str(fd, "\n*** OpenSupCom crashed: ");
    write_str(fd, signal_name(sig));
    char number[24];
    // (A real fault's: a signal sent by raise or kill, si_code <= 0, has none)
    if (info && info->si_code > 0 && (sig == SIGSEGV || sig == SIGBUS)) {
        format_number(number, reinterpret_cast<std::uintptr_t>(info->si_addr), 16);
        write_str(fd, ", fault address ");
        write_str(fd, number);
    }
    if (pc != 0) {
        format_number(number, pc, 16);
        write_str(fd, ", pc ");
        write_str(fd, number);
        if (pc >= g_image_begin && pc < g_image_end) {
            format_number(number, pc - g_image_bias, 16);
            write_str(fd, " (addr2line -e <binary> ");
            write_str(fd, number);
            write_str(fd, ")");
        }
    }
    write_str(fd, " ***\nBacktrace (resolve offsets with addr2line -e <binary>; none below "
                  "if the stack couldn't be walked):\n");
}

/// Open crash-<time>-<pid>.txt in the report folder, its path into `path`;
/// -1 when there is no folder or it can't be made.
int open_report_file(char* path, size_t size) {
    if (g_report_dir[0] == '\0') return -1;
    timespec now{};
    clock_gettime(CLOCK_REALTIME, &now);
    char seconds[24];
    char pid[24];
    format_number(seconds, static_cast<std::uintmax_t>(now.tv_sec), 10);
    format_number(pid, static_cast<std::uintmax_t>(getpid()), 10);
    const char* parts[] = {g_report_dir, "crash-", seconds, "-", pid, ".txt"};
    size_t used = 0;
    for (const char* part : parts) {
        const size_t n = std::strlen(part);
        if (used + n + 1 > size) return -1;
        std::memcpy(path + used, part, n);
        used += n;
    }
    path[used] = '\0';
    return open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
}

void fatal_signal_handler(int sig, siginfo_t* info, void* context) {
    // A second fault while reporting (or a fault on another thread) goes
    // straight to the default action.
    if (g_in_handler.exchange(true)) {
        signal(sig, SIG_DFL);
        raise(sig);
        return;
    }

    // What and where first, to the file and stderr, then the backtrace. A
    // wild call leaves a stack the unwinder faults on, and that fault kills
    // the process at once (SA_RESETHAND): the summary is already out.
    const std::uintptr_t pc = fault_pc(context);
    char path[sizeof(g_report_dir) + 64];
    const int fd = open_report_file(path, sizeof(path));
    if (fd >= 0) {
        write_str(fd, g_report_header);
        write_summary(fd, sig, info, pc);
        write_str(STDERR_FILENO, "Crash report: ");
        write_str(STDERR_FILENO, path);
        write_str(STDERR_FILENO, "\n");
    }
    write_summary(STDERR_FILENO, sig, info, pc);

    void* frames[64];
    const int count = g_unwinder(frames, 64);
    if (fd >= 0) {
        backtrace_symbols_fd(frames, count, fd);
        close(fd);
    }
    backtrace_symbols_fd(frames, count, STDERR_FILENO);

    // SA_RESETHAND already restored the default action; re-raise so the
    // process still dies by this signal (core dump, correct exit status).
    signal(sig, SIG_DFL);
    raise(sig);
}

} // namespace

void install_crash_handler() {
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
#ifdef __linux__
    dl_iterate_phdr(find_main_image, nullptr);
#endif

    struct sigaction sa{};
    sa.sa_sigaction = fatal_signal_handler;
    // SA_NODEFER: a fault in the handler (an unwinder on a smashed stack)
    // must meet the default action SA_RESETHAND restored, not a blocked
    // signal; Linux kills the process either way, macOS re-runs the
    // faulting instruction forever.
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    for (int sig : kFatalSignals) {
        sigaction(sig, &sa, nullptr);
    }

    signal(SIGPIPE, SIG_IGN);
}

void set_unwinder_for_test(UnwinderForTest unwinder) {
    g_unwinder = unwinder ? unwinder : &backtrace;
}

void crash_for_test() {
    std::raise(SIGSEGV);
    std::abort(); // (the handler re-raises; this is never reached)
}

#endif

} // namespace osc::platform
