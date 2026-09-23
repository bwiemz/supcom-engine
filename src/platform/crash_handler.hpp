#pragma once

namespace osc::platform {

/// Called from the crash handler after the backtrace is written, to flush
/// logs. It runs in a signal / SEH context, so it is best-effort only.
using CrashFlushHook = void (*)();

/// Install process-wide crash reporting. Idempotent; the last flush hook wins.
///
/// POSIX: SIGSEGV/SIGBUS/SIGFPE/SIGILL/SIGABRT write a report and backtrace
/// to stderr (on an alternate stack, so stack overflows are reported too),
/// run `flush`, then re-raise with the default action so core dumps and the
/// "killed by signal" exit status are preserved. SIGPIPE is ignored: a peer
/// that vanishes must surface as a failed send, not kill the game.
///
/// Windows: an unhandled-exception filter logs the exception code and
/// address, runs `flush`, and lets the process terminate.
void install_crash_handler(CrashFlushHook flush = nullptr);

} // namespace osc::platform
