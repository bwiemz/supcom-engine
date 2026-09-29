#include "core/log.hpp"
#include "core/test_status.hpp"
#include "core/version.hpp"

#include <spdlog/sinks/stdout_color_sinks.h>

#include <string>
#include <system_error>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

namespace osc::log {

namespace {

std::shared_ptr<DeferredFileSink> g_file_sink;

} // namespace

bool DeferredFileSink::open(const std::filesystem::path& file) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (open_) return true;
    try {
        file_.open(file.string(), /*truncate=*/true);
    } catch (const spdlog::spdlog_ex&) {
        return false;
    }
    open_ = true;
    file_.write(held_);
    held_ = spdlog::memory_buf_t();
    file_.flush();
    return true;
}

bool DeferredFileSink::is_open() {
    const std::lock_guard<std::mutex> lock(mutex_);
    return open_;
}

void DeferredFileSink::sink_it_(const spdlog::details::log_msg& msg) {
    spdlog::memory_buf_t formatted;
    formatter_->format(msg, formatted);
    if (open_) file_.write(formatted);
    else if (held_.size() + formatted.size() <= kHeldMax)
        held_.append(formatted.data(), formatted.data() + formatted.size());
}

void DeferredFileSink::flush_() {
    if (open_) file_.flush();
}

void init() {
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    g_file_sink = std::make_shared<DeferredFileSink>();

    auto logger =
        std::make_shared<spdlog::logger>("osc", spdlog::sinks_init_list{console_sink, g_file_sink});
    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
    logger->set_level(spdlog::level::debug);
    // Warnings and errors reach the file immediately, so a crash (whose
    // handler must not touch the logger) never loses them.
    logger->flush_on(spdlog::level::warn);

    spdlog::set_default_logger(logger);
    spdlog::info("{}", core::version_line());
}

bool open_file(const std::filesystem::path& file) {
    if (!g_file_sink) return false;
    std::error_code ec;
    if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), ec);
    if (g_file_sink->open(file)) return true;
    spdlog::warn("Can't write the log to {}: it stays on the console", file.string());
    return false;
}

bool file_open() {
    return g_file_sink && g_file_sink->is_open();
}

void rotate(const std::filesystem::path& file, int keep) {
    namespace fs = std::filesystem;
    const auto numbered = [&](int n) {
        fs::path p = file;
        p.replace_filename(file.stem().string() + "." + std::to_string(n) +
                           file.extension().string());
        return p;
    };
    std::error_code ec;
    if (!fs::exists(file, ec)) return; // no run to keep: leave the older ones be
    for (int n = keep; n >= 1; --n) {
        const fs::path from = n == 1 ? file : numbered(n - 1);
        if (fs::exists(from, ec)) fs::rename(from, numbered(n), ec);
    }
}

void shutdown() {
    spdlog::shutdown();
    g_file_sink.reset();
}

/// Concatenate all Lua arguments into a single string, mimicking the original
/// engine's LOG/WARN/SPEW behavior.
static std::string lua_concat_args(lua_State* L) {
    int n = lua_gettop(L);
    std::string result;
    for (int i = 1; i <= n; i++) {
        if (lua_isstring(L, i)) {
            result += lua_tostring(L, i);
        } else if (lua_isnil(L, i)) {
            result += "nil";
        } else if (lua_isboolean(L, i)) {
            result += lua_toboolean(L, i) ? "true" : "false";
        } else if (lua_isnumber(L, i)) {
            result += lua_tostring(L, i);
        } else {
            result += lua_typename(L, lua_type(L, i));
        }
    }
    return result;
}

static bool is_builder_deepcopy_diagnostic(const std::string& message) {
    return message.find("stack traceback:") != std::string::npos &&
           message.find("/lua/system/utils.lua:158: in function `deepcopy'") !=
               std::string::npos &&
           message.find("/lua/sim/builder.lua:234: in function "
                        "`SetupBuilderConditions'") != std::string::npos;
}

/// In test modes, a Lua LOG/WARN line containing the uppercase token "FAIL"
/// ("X TEST FAILED: ...", "Bone test 1: FAIL - ...") is the embedded Lua
/// tests' way of reporting a failed check. FA's own scripts never log it.
static void note_lua_test_failure(const std::string& message) {
    if (test_status::count_lua_failures() &&
        message.find("FAIL") != std::string::npos) {
        test_status::record_failure(message);
    }
}

int l_LOG(lua_State* L) {
    auto message = lua_concat_args(L);
    spdlog::info("{}", message);
    note_lua_test_failure(message);
    return 0;
}

int l_WARN(lua_State* L) {
    auto message = lua_concat_args(L);
    note_lua_test_failure(message);
    if (is_builder_deepcopy_diagnostic(message)) {
        spdlog::trace("{}", message);
    } else {
        spdlog::warn("{}", message);
    }
    return 0;
}

int l_SPEW(lua_State* L) {
    spdlog::debug("{}", lua_concat_args(L));
    return 0;
}

int l_ALERT(lua_State* L) {
    spdlog::error("{}", lua_concat_args(L));
    return 0;
}

} // namespace osc::log
