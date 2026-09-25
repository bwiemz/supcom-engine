#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace osc::lua {

class LuaState;

/// What OpenURL may open and how: the init file's `protocols` and the
/// system's URL handler (platform::open_url; tests pass their own).
struct UrlOpener {
    std::vector<std::string> protocols;
    std::function<void(const std::string&)> open;
};

/// The scheme of `url` as a URL parser reads it: the letters, digits, '+',
/// '-' and '.' before its first ':', starting with a letter. Empty when it
/// has none, or when the URL holds a space or a control character.
std::string url_scheme(std::string_view url);

/// Register OpenURL(url) on a UI Lua state, as Moho's user state has it
/// (faf-re cfunc_OpenURLL): one string argument; a URL that parses and
/// whose scheme is one of `opener`'s protocols (in any case) goes to its
/// handler, and any other does nothing. `opener` must outlive the state.
void register_url_bindings(LuaState& state, UrlOpener* opener);

} // namespace osc::lua
