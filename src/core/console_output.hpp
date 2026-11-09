#pragma once

#include <functional>
#include <string>
#include <string_view>

struct lua_State;

namespace osc::core {

int add_console_receiver(lua_State* L, int index);

void remove_console_receiver(lua_State* L, int handle);

/// Moho's CON_Printf: `line` goes to each of the state's receivers, in the
/// order they were added.
void console_print(lua_State* L, std::string_view line);

/// Moho's SCR_ConcatArgsAndCall: the arguments as one text, `delimiter`
/// between them, handed to `emit` line by line.
void concat_print_args(lua_State* L, unsigned char delimiter,
                       const std::function<void(const std::string&)>& emit);

} // namespace osc::core
