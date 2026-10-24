#pragma once

// A script the engine runs for an entity raised: Moho's
// CScriptObject::LogScriptWarning (faf-re CScriptObject.cpp, 0x004C7410),
// which each of its script calls (RunScript, GetWeaponClass) logs before its
// own answer stands in for the script's.

#include "core/test_status.hpp"
#include "sim/entity.hpp"

#include <spdlog/spdlog.h>

#include <cstdint>
#include <string>

namespace osc::sim {

/// "Error running <which> script in <where>: <message>", `where` being
/// Moho's Entity::GetErrorDescription ("Entity <blueprint id> at
/// <address>"), or "<deleted object>" for none. A test mode counts it as a
/// failure, as it does a script thread's error.
inline void log_script_warning(const Entity* entity, const char* which, const char* message) {
    const std::string where = entity ? fmt::format("Entity {} at {:08x}", entity->blueprint_id(),
                                                   reinterpret_cast<std::uintptr_t>(entity))
                                     : std::string("<deleted object>");
    const std::string line =
        fmt::format("Error running {} script in {}: {}", which, where, message ? message : "");
    spdlog::warn("{}", line);
    if (test_status::count_lua_failures()) test_status::record_failure(line);
}

} // namespace osc::sim
