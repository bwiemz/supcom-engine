#pragma once

#include <string>
#include <string_view>

struct lua_State;

namespace osc::sim {

/// The script module beside a blueprint file: "<dir>/<id>_unit.bp" ->
/// "<dir>/<id>_script.lua" when `source` ends in `bp_suffix` ("_unit.bp",
/// "_proj.bp"); empty otherwise. Lowercased, as the VFS keys paths.
std::string default_script_module(std::string source, std::string_view bp_suffix);

/// Push the Lua class a blueprint's objects are instances of -- its
/// ScriptModule/ScriptClass, else the default module beside its .bp and
/// TypeClass -- or nil when it names none that loads. Resolved once per
/// blueprint, cached in the registry under `cache_key` (a failure is
/// logged once, then cached). `kind` names the objects in the log. Most
/// props have no script beside their .bp, so for them a missing default
/// module isn't worth a warning (`warn_default_missing` false).
void push_blueprint_script_class(lua_State* L, const std::string& bp_id, std::string_view bp_suffix,
                                 const char* cache_key, const char* kind,
                                 bool warn_default_missing = true);

/// Replace the class on top of the stack with a new object of it, made as
/// Moho's CScriptObject::CreateLuaObject makes an entity's (faf-re): by
/// calling the class, with no arguments, when its metatable has a __call
/// (class.lua's runs __init and __post_init; FAF's ACUs name their gun in
/// __init), else a new table with the class as its metatable. When the
/// call fails or returns no table, a warning names `kind` and the object
/// is the plain instance. A non-table on top becomes an empty table.
void push_new_script_object(lua_State* L, const char* kind);

} // namespace osc::sim
