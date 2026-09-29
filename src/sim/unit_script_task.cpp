// Script orders (M206w): a unit's Lua task, as Moho's CUnitScriptTask runs
// one. The order's table names its class (TaskName); the engine makes the
// object, runs OnCreate(args), then its TaskTick as long as it asks, and
// OnDestroy at its end. See docs/plans/2026-09-29-m206w-script-orders-design.md.

#include "core/test_status.hpp"
#include "sim/lua_bytes.hpp"
#include "sim/script_class.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <string>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace osc::sim {

namespace {

// TaskTick's statuses: Moho's task codes, which retail's ScriptTask.lua
// mirrors as TASKSTATUS. Wait is 1; n > 1 runs again n - 1 ticks later
// (see the moho-task-timing notes).
constexpr int kTaskDone = -1;
constexpr int kTaskSuspend = -2;
constexpr int kTaskDelay = -4; // (Abort is -3: it ends the order, as Done does)
constexpr int kTaskRepeat = 0;

/// How often a task may repeat within one tick before it has to wait for
/// the next: Moho would spin for ever.
constexpr int kMaxRepeats = 64;

void warn(const std::string& message) {
    spdlog::warn("{}", message);
    if (test_status::count_lua_failures()) test_status::record_failure(message);
}

/// Push the class a task of `task_name` is made from:
/// /lua/sim/tasks/<name>.lua's <name>, else retail's ScriptTask itself (Moho
/// logs "Can't find task %s, using ScriptTask directly"). Nil if neither.
void push_task_class(lua_State* L, const std::string& task_name) {
    const int top = lua_gettop(L);
    const auto import = [&](const std::string& path, const std::string& name) {
        lua_settop(L, top);
        lua_pushstring(L, "import");
        lua_rawget(L, LUA_GLOBALSINDEX);
        if (!lua_isfunction(L, -1)) return false;
        lua_pushstring(L, path.c_str());
        if (lua_pcall(L, 1, 1, 0) != 0 || !lua_istable(L, -1)) return false;
        lua_pushstring(L, name.c_str());
        lua_gettable(L, -2);
        if (!lua_istable(L, -1)) return false;
        lua_replace(L, top + 1);
        lua_settop(L, top + 1);
        return true;
    };
    if (!task_name.empty() && import("/lua/sim/tasks/" + task_name + ".lua", task_name)) return;
    spdlog::info("Can't find task {}, using ScriptTask directly", task_name);
    if (import("/lua/sim/ScriptTask.lua", "ScriptTask")) return;
    lua_settop(L, top);
    lua_pushnil(L);
}

/// Call `obj`'s `method` (found through its class) with `arg` if it is a
/// stack index (0: none). Warns on an error.
void call_task(lua_State* L, int obj, const char* method, int arg) {
    lua_pushstring(L, method);
    lua_gettable(L, obj);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    lua_pushvalue(L, obj);
    if (arg != 0) lua_pushvalue(L, arg);
    if (lua_pcall(L, arg != 0 ? 2 : 1, 0, 0) != 0) {
        warn(fmt::format("Script task {} error: {}", method,
                         lua_isstring(L, -1) ? lua_tostring(L, -1) : "?"));
        lua_pop(L, 1);
    }
}

} // namespace

bool Unit::start_script_task(UnitCommand& cmd, lua_State* L) {
    const int top = lua_gettop(L);
    // The order's table (its TaskName names the class); none: an empty one
    if (!push_lua_bytes(L, cmd.script_args) || !lua_istable(L, -1)) {
        lua_settop(L, top);
        lua_newtable(L);
    }
    const int args = lua_gettop(L);
    std::string task_name;
    lua_pushstring(L, "TaskName");
    lua_rawget(L, args);
    if (lua_type(L, -1) == LUA_TSTRING) task_name = lua_tostring(L, -1);
    lua_pop(L, 1);

    push_task_class(L, task_name);
    push_new_script_object(L, "Script task");
    const int obj = lua_gettop(L);
    // The object keeps its unit's (GetUnit): still there in OnDestroy when
    // the unit is being destroyed, as Moho's task reaches its unit then.
    lua_pushstring(L, "_c_unit");
    lua_rawgeti(L, LUA_REGISTRYINDEX, lua_table_ref());
    lua_rawset(L, obj);

    script_task_ = ScriptTaskRun{};
    cmd.task_serial = ++next_task_serial_;
    script_task_.serial = cmd.task_serial;
    lua_pushvalue(L, obj);
    script_task_.object_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    call_task(L, obj, "OnCreate", args);
    lua_settop(L, top);
    return !destroyed() && in_registry();
}

int Unit::tick_script_task(lua_State* L) {
    const int top = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, script_task_.object_ref);
    const int obj = lua_gettop(L);
    // Through the object: a state machine's current state has its own
    lua_pushstring(L, "TaskTick");
    lua_gettable(L, obj);
    if (!lua_isfunction(L, -1)) {
        lua_settop(L, top);
        return kTaskRepeat; // Moho's: none is 0
    }
    lua_pushvalue(L, obj);
    int status = kTaskDone;
    if (lua_pcall(L, 1, 1, 0) != 0) {
        warn(fmt::format("Script task TaskTick error: {}",
                         lua_isstring(L, -1) ? lua_tostring(L, -1) : "?"));
    } else if (lua_type(L, -1) == LUA_TNUMBER) {
        status = static_cast<int>(lua_tonumber(L, -1));
    } else {
        warn("Script task TaskTick returned no status: it ends");
    }
    lua_settop(L, top);
    return status;
}

void Unit::end_script_task(lua_State* L) {
    if (!has_script_task()) return;
    const int ref = script_task_.object_ref;
    // Over before its OnDestroy runs, which may give orders or end the unit
    script_task_ = ScriptTaskRun{};
    if (!L) return;
    const int top = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    if (lua_istable(L, -1)) call_task(L, lua_gettop(L), "OnDestroy", 0);
    lua_settop(L, top);
    luaL_unref(L, LUA_REGISTRYINDEX, ref);
}

OrderStep Unit::order_script(UnitCommand& cmd, SimContext& ctx) {
    lua_State* L = ctx.L;
    if (!L || lua_table_ref() < 0) {
        command_queue_.pop_front();
        return OrderStep::Next;
    }
    if (!has_script_task() || script_task_.serial != cmd.task_serial) {
        end_script_task(L); // (another order's: already gone from the front)
        if (!start_script_task(cmd, L)) return OrderStep::Gone;
    }
    const u32 serial = script_task_.serial;
    // Its order still at the front, and the task still its own (a script
    // may clear the queue, or end the task, under it)
    const auto still_running = [&] {
        return has_script_task() && script_task_.serial == serial && !command_queue_.empty() &&
               command_queue_.front().task_serial == serial;
    };
    if (!still_running()) {
        end_script_task(L);
        return destroyed() || !in_registry() ? OrderStep::Gone : OrderStep::Next;
    }
    if (script_task_.suspended) return OrderStep::Hold;
    if (script_task_.wait > 0) {
        --script_task_.wait;
        return OrderStep::Hold;
    }

    for (int repeat = 0; repeat < kMaxRepeats; ++repeat) {
        const int status = tick_script_task(L);
        if (destroyed() || !in_registry()) return OrderStep::Gone;
        if (!still_running()) {
            end_script_task(L);
            return destroyed() || !in_registry() ? OrderStep::Gone : OrderStep::Next;
        }
        if (status == kTaskRepeat) continue;
        if (status == kTaskSuspend) {
            script_task_.suspended = true;
            return OrderStep::Hold;
        }
        if (status > 0 || status == kTaskDelay) {
            // Wait (1) and Delay: next tick; n > 1: n - 1 ticks on
            script_task_.wait = status > 1 ? static_cast<u32>(status - 2) : 0;
            return OrderStep::Hold;
        }
        // Done, Abort, or a code no task gives: the order ends
        end_script_task(L);
        if (destroyed() || !in_registry()) return OrderStep::Gone;
        if (!command_queue_.empty() && command_queue_.front().task_serial == serial)
            command_queue_.pop_front();
        return OrderStep::Next;
    }
    warn(fmt::format("Script task of unit {} repeated {} times in a tick", entity_id(),
                     kMaxRepeats));
    return OrderStep::Hold;
}

} // namespace osc::sim
