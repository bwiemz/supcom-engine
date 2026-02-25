#include "core/log.hpp"

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

int l_LOG(lua_State* L) {
    spdlog::info("{}", lua_concat_args(L));
    return 0;
}

int l_WARN(lua_State* L) {
    spdlog::warn("{}", lua_concat_args(L));
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
