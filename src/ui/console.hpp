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
    /// CON_GetFindTextMatches: the names starting with `prefix`, in any case.
    std::vector<std::string> matches(std::string_view prefix) const;

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

/// Moho's console variables (TConVar, M217i): the value a command's
/// `args` (the variable's name first) make of `value`.
/// - bool: no argument toggles; on/true, off/false, tog, `= n`, or a bare
///   number (not 0 is on); `show` only shows it.
/// - int: `= += -= *= /= %= &= |= ^=` and a value, on/true (1),
///   off/false (0), tog, or a bare value; no argument only shows it.
/// - float: as int, without `%= &= |= ^=`, on/off and tog.
/// Values parse as atoi/atof (junk is 0); a division by 0 leaves the value.
/// `shown` says the command only showed the value.
bool convar_bool(const std::vector<std::string>& args, bool value, bool& shown);
int convar_int(const std::vector<std::string>& args, int value, bool& shown);
float convar_float(const std::vector<std::string>& args, float value, bool& shown);

/// A console variable on `console`, read through `get` and written through
/// `set` in the UI state the command runs in, with Moho's syntax and
/// messages ("toggled X is now on", "int X == 3", "float X == 0.0500").
void add_bool_var(Console& console, const std::string& name, std::function<bool(lua_State*)> get,
                  std::function<void(lua_State*, bool)> set);
void add_int_var(Console& console, const std::string& name, std::function<int(lua_State*)> get,
                 std::function<void(lua_State*, int)> set);
void add_float_var(Console& console, const std::string& name, std::function<float(lua_State*)> get,
                   std::function<void(lua_State*, float)> set);

} // namespace osc::ui
