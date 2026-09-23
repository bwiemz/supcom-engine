#include <catch2/catch_test_macros.hpp>

#include "platform/crash_handler.hpp"

#ifndef _WIN32

#include <csignal>
#include <string>
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

#endif
