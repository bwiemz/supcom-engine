#pragma once

#include <filesystem>
#include <memory>
#include <mutex>
#include <spdlog/details/file_helper.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/spdlog.h>

// Forward declare lua_State to avoid pulling in Lua headers everywhere
struct lua_State;

namespace osc::log {

/// Start logging: to the console at once, and to a file once open_file
/// names one (M228b). Until then the file's lines are held, so what a run
/// logs before it knows where its log belongs isn't lost.
void init();

/// Send the log to `file` (created, or truncated), starting with the lines
/// held so far. False, with a warning on the console, when it can't be
/// opened: the log then stays on the console. Once a file is open, later
/// calls change nothing.
bool open_file(const std::filesystem::path& file);

/// Whether the log has a file (open_file succeeded).
bool file_open();

/// Keep the last `keep` runs' logs before a new one starts: `file` becomes
/// <stem>.1<ext>, .1 becomes .2, and so on, the oldest dropped. With no
/// `file` nothing moves; a rename that fails (another instance holding the
/// file, on Windows) is ignored.
void rotate(const std::filesystem::path& file, int keep);

/// Flush and shutdown logging.
void shutdown();

/// A file sink that holds its lines until it is opened on a file
/// (log::init's). At most kHeldMax bytes are held; more are dropped.
class DeferredFileSink final : public spdlog::sinks::base_sink<std::mutex> {
public:
    static constexpr size_t kHeldMax = 8u << 20;

    /// Open `file` (truncating it) and write the held lines. True if the
    /// sink has a file (now, or already).
    bool open(const std::filesystem::path& file);
    bool is_open();

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override;
    void flush_() override;

private:
    spdlog::details::file_helper file_;
    spdlog::memory_buf_t held_;
    bool open_ = false;
};

// Lua-side logging functions (C functions registered into Lua)
int l_LOG(lua_State* L);
int l_WARN(lua_State* L);
int l_SPEW(lua_State* L);
int l_ALERT(lua_State* L);

} // namespace osc::log
