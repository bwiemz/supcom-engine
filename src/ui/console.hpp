#pragma once

#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

struct lua_State;

namespace osc::ui {

/// Parse the first command of a console line as Moho's CON_ParseCommand
/// does: whitespace separates tokens; a token that begins with `"` runs to
/// the next `"` (`\"` and `\\` escape inside it; a quote mid-token is just a
/// character); `#` or `//` ends the line; `;` ends the command, and what
/// follows it goes to `remainder`.
void parse_console_command(std::string_view line, std::vector<std::string>& tokens,
                           std::string& remainder);

/// Moho's console: named commands, found case-insensitively, run by line.
/// Key map actions (`UI_Lua ...`) and ConExecute reach the engine through it.
class Console {
public:
    /// A command's handler: the UI Lua state it runs in, and its tokens (the
    /// name first).
    using Handler = std::function<void(lua_State* L, const std::vector<std::string>& args)>;

    void add(std::string name, Handler handler);
    bool has(std::string_view name) const;

    /// CON_Execute: each `;`-separated command in turn. An unknown one is
    /// logged, as Moho prints it.
    void execute(lua_State* L, std::string_view line);

private:
    struct NoCase {
        using is_transparent = void;
        bool operator()(std::string_view a, std::string_view b) const;
    };
    std::map<std::string, Handler, NoCase> commands_;
};

} // namespace osc::ui
