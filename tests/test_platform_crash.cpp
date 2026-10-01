#include <catch2/catch_test_macros.hpp>

#include "platform/crash_handler.hpp"

#ifndef _WIN32

#include <csignal>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

namespace {

struct ChildResult {
    int status = 0;
    std::string stderr_text;
};

/// Fork; in the child, redirect stderr into a pipe, install the handler and
/// run `body`. Returns how the child ended and what it wrote to stderr.
template <typename Body>
ChildResult run_in_child(Body body) {
    int fds[2];
    REQUIRE(pipe(fds) == 0);
    pid_t pid = fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDERR_FILENO);
        osc::platform::install_crash_handler();
        body();
        _exit(0);
    }
    close(fds[1]);
    ChildResult result;
    char buf[4096];
    ssize_t n;
    while ((n = read(fds[0], buf, sizeof(buf))) > 0) {
        result.stderr_text.append(buf, static_cast<size_t>(n));
    }
    close(fds[0]);
    waitpid(pid, &result.status, 0);
    return result;
}

/// Store to a small address: a real fault, SEGV_MAPERR at `address`.
void fault_at(std::uintptr_t address) {
    // A fault on purpose, at an address no mapping holds (volatile keeps
    // the store): the checks flag exactly that
    // NOLINTNEXTLINE(performance-no-int-to-ptr,clang-analyzer-core.FixedAddressDereference)
    *reinterpret_cast<volatile int*>(address) = 1;
}

/// An unwinder that faults, as backtrace() does on a stack a wild call
/// smashed.
int faulting_unwinder(void** /*frames*/, int /*size*/) {
    fault_at(16);
    return 0;
}

/// The one report in `dir`, and its text; empty if there isn't exactly one.
std::string only_report(const std::filesystem::path& dir) {
    std::vector<std::filesystem::path> reports;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
        reports.push_back(entry.path());
    if (reports.size() != 1) return {};
    std::ifstream in(reports[0]);
    return {(std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()};
}

std::filesystem::path fresh_dir() {
    std::random_device rd;
    return std::filesystem::temp_directory_path() /
           ("osc_crash_test_" + std::to_string(rd()) + std::to_string(rd()));
}

} // namespace

TEST_CASE("crash handler reports a fatal signal, then dies by it", "[platform]") {
    auto result = run_in_child([] { std::raise(SIGSEGV); });

    // The default action still runs afterwards, so core dumps and the
    // "killed by signal" exit status are preserved.
    REQUIRE(WIFSIGNALED(result.status));
    CHECK(WTERMSIG(result.status) == SIGSEGV);
    CHECK(result.stderr_text.find("OpenSupCom crashed") != std::string::npos);
    CHECK(result.stderr_text.find("SIGSEGV") != std::string::npos);
    CHECK(result.stderr_text.find("Backtrace") != std::string::npos);
}

TEST_CASE("crash handler also writes its report into the report folder", "[platform]") {
    std::random_device rd;
    const auto dir = std::filesystem::temp_directory_path() /
                     ("osc_crash_test_" + std::to_string(rd()) + std::to_string(rd()));
    auto result = run_in_child([&] {
        osc::platform::set_crash_report_dir(dir, "OpenSupCom 9.9.9 (test)\nLog: /x.log");
        std::raise(SIGSEGV);
    });
    REQUIRE(WIFSIGNALED(result.status));
    CHECK(WTERMSIG(result.status) == SIGSEGV);

    std::vector<std::filesystem::path> reports;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
        reports.push_back(entry.path());
    REQUIRE(reports.size() == 1);
    const std::string name = reports[0].filename().string();
    CHECK(name.starts_with("crash-"));
    CHECK(name.ends_with(".txt"));
    std::ifstream in(reports[0]);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(text.starts_with("OpenSupCom 9.9.9 (test)\nLog: /x.log\n"));
    CHECK(text.find("SIGSEGV") != std::string::npos);
    CHECK(text.find("Backtrace") != std::string::npos);
#ifndef __APPLE__
    // raise() sent it: no fault address to report. (macOS can't tell: it gives
    // a sent SIGSEGV a fault's si_code, SEGV_ACCERR, at address 0)
    CHECK(text.find("fault address") == std::string::npos);
#endif
    CHECK(result.stderr_text.find("Crash report: " + reports[0].string()) != std::string::npos);
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("crash handler ignores SIGPIPE", "[platform]") {
    auto result = run_in_child([] { std::raise(SIGPIPE); });
    REQUIRE(WIFEXITED(result.status));
    CHECK(WEXITSTATUS(result.status) == 0);
}

TEST_CASE("install_crash_handler is idempotent", "[platform]") {
    auto result = run_in_child([] {
        osc::platform::install_crash_handler();
        osc::platform::install_crash_handler();
        std::raise(SIGABRT);
    });
    REQUIRE(WIFSIGNALED(result.status));
    CHECK(WTERMSIG(result.status) == SIGABRT);
    // One report, not one per install.
    const auto& text = result.stderr_text;
    auto first = text.find("OpenSupCom crashed");
    REQUIRE(first != std::string::npos);
    CHECK(text.find("OpenSupCom crashed", first + 1) == std::string::npos);
}

TEST_CASE("crash handler reports a fault's address and where it ran", "[platform]") {
    auto result = run_in_child([] { fault_at(8); });
    REQUIRE(WIFSIGNALED(result.status));
    CHECK(WTERMSIG(result.status) == SIGSEGV);
    CHECK(result.stderr_text.find("fault address 0x8") != std::string::npos);
#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
    // The faulting instruction, in the binary (fault_at is in it)
    CHECK(result.stderr_text.find(", pc 0x") != std::string::npos);
    CHECK(result.stderr_text.find("(addr2line -e <binary> 0x") != std::string::npos);
#endif
}

TEST_CASE("a stack the unwinder can't walk still leaves what crashed and where", "[platform]") {
    const auto dir = fresh_dir();
    auto result = run_in_child([&] {
        osc::platform::set_crash_report_dir(dir, "OpenSupCom 9.9.9 (test)");
        osc::platform::set_unwinder_for_test(faulting_unwinder);
        fault_at(8);
    });
    // The unwinder's own fault ends it, as on a smashed stack
    REQUIRE(WIFSIGNALED(result.status));
    CHECK(WTERMSIG(result.status) == SIGSEGV);
    const std::string text = only_report(dir);
    CHECK(text.starts_with("OpenSupCom 9.9.9 (test)\n"));
    CHECK(text.find("SIGSEGV") != std::string::npos);
    CHECK(text.find("fault address 0x8") != std::string::npos);
#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
    CHECK(text.find(", pc 0x") != std::string::npos);
#endif
    // ... and nothing after the backtrace's heading: the walk never finished
    const auto heading = text.find("Backtrace");
    REQUIRE(heading != std::string::npos);
    CHECK(text.find('\n', heading) == text.size() - 1);
    CHECK(result.stderr_text.find("fault address 0x8") != std::string::npos);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

#endif
