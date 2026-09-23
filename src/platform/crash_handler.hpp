#pragma once

namespace osc::platform {

/// Install process-wide crash reporting. Idempotent.
///
/// POSIX: SIGSEGV/SIGBUS/SIGFPE/SIGILL/SIGABRT write a report and backtrace
/// to stderr (on an alternate stack, so stack overflows are reported too),
/// then re-raise with the default action so core dumps and the "killed by
/// signal" exit status are preserved. SIGPIPE is ignored: a peer that
/// vanishes must surface as a failed send, not kill the game.
///
/// Windows: an unhandled-exception filter reports the exception code and
/// address to stderr and lets the process terminate.
///
/// The handler deliberately does not touch the logger: a fault inside a
/// logging call would deadlock on the logger's mutex. Loggers should flush
/// warnings and errors eagerly instead (see osc::log::init).
void install_crash_handler();

} // namespace osc::platform
