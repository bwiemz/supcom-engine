#pragma once

#include <spdlog/spdlog.h>

#include <string>
#include <utility>
#include <vector>

/// Process-wide pass/fail tally for the integration test modes
/// (`opensupcom --<name>-test`). Test code reports through fail(); main()
/// turns the tally into the process exit code so CTest and CI can gate on it.
namespace osc::test_status {

/// Record one failure (the message is kept for the end-of-run summary).
void record_failure(std::string message);

/// Number of failures recorded so far.
int failure_count();

/// The first recorded failure messages (capped), in order.
std::vector<std::string> failure_messages();

/// Forget all recorded failures.
void reset();

/// Embedded Lua tests report by LOG/WARN-ing a line containing "FAIL"
/// ("... FAILED", ": FAIL - ..."). When enabled (test modes only), such lines
/// count as failures. Off by default so normal play never trips it.
void set_count_lua_failures(bool enabled);
bool count_lua_failures();

/// Log an error and record it as a test failure.
template <typename... Args>
void fail(spdlog::format_string_t<Args...> fmt, Args&&... args) {
    std::string message = fmt::format(fmt, std::forward<Args>(args)...);
    spdlog::error("{}", message);
    record_failure(std::move(message));
}

} // namespace osc::test_status
