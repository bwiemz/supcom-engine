#include "core/log.hpp"
#include "core/test_status.hpp"

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

namespace osc::log {

void init(const std::filesystem::path& log_file) {
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
        log_file.string(), true);

    auto logger = std::make_shared<spdlog::logger>(
        "osc", spdlog::sinks_init_list{console_sink, file_sink});
    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
    logger->set_level(spdlog::level::debug);

    spdlog::set_default_logger(logger);
    spdlog::info("OpenSupCom v0.1.0");
}

void shutdown() {
    spdlog::shutdown();
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
