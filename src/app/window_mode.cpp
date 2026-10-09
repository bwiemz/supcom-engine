// The window as FA opens and sets it (M217h), from faf-re's CScApp
// (the startup head), StartupHelpers (CFG_GetArgOption's prefixes and
// aliases, SetupPrimaryAdapterSettings) and ResolutionCommands.

#include "app/window_mode.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <set>
#include <tuple>

namespace osc::app {

namespace {

/// Parse a decimal integer, whole.
std::optional<i64> integer(std::string_view s) {
    while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
    while (!s.empty() && s.back() == ' ') s.remove_suffix(1);
    i64 v = 0;
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc() || end != s.data() + s.size()) return std::nullopt;
    return v;
}

bool same_no_case(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) ==
                      std::tolower(static_cast<unsigned char>(y));
           });
}

/// CFG_GetArgOption: an argument that is one of `aliases` behind one of
/// Moho's prefixes (/, -, +, \; the engine's -- too), and the `count`
/// arguments after it.
std::optional<std::vector<std::string>> arg_option(const std::vector<std::string>& args,
                                                   std::initializer_list<std::string_view> aliases,
                                                   size_t count) {
    for (size_t i = 0; i < args.size(); ++i) {
        std::string_view a = args[i];
        if (a.empty() || std::string_view("/-+\\").find(a.front()) == std::string_view::npos)
            continue;
        a.remove_prefix(a.size() > 1 && a[0] == '-' && a[1] == '-' ? 2 : 1);
        const bool named = std::any_of(aliases.begin(), aliases.end(), [&](std::string_view alias) {
            return same_no_case(a, alias);
        });
        if (!named || i + count >= args.size()) continue;
        return std::vector<std::string>(args.begin() + static_cast<std::ptrdiff_t>(i + 1),
                                        args.begin() + static_cast<std::ptrdiff_t>(i + 1 + count));
    }
    return std::nullopt;
}

/// ClampAtLeast of a parsed argument (atoi: junk is 0).
u32 at_least(u32 least, const std::string& text) {
    const auto v = integer(text);
    return static_cast<u32>(std::max<i64>(least, v.value_or(0)));
}

} // namespace

std::optional<Resolution> parse_resolution(std::string_view text) {
    std::array<std::string_view, 3> parts{};
    size_t n = 0;
    while (n < parts.size()) {
        const size_t comma = text.find(',');
        parts[n++] = text.substr(0, comma);
        if (comma == std::string_view::npos) break;
        text.remove_prefix(comma + 1);
        if (n == parts.size()) return std::nullopt; // a fourth part
    }
    if (n < 2) return std::nullopt;
    const auto w = integer(parts[0]);
    const auto h = integer(parts[1]);
    const auto r = n == 3 ? integer(parts[2]) : std::optional<i64>(60);
    if (!w || !h || !r || *w <= 0 || *h <= 0 || *r < 0) return std::nullopt;
    return Resolution{static_cast<u32>(*w), static_cast<u32>(*h), static_cast<u32>(*r)};
}

WindowMode startup_window_mode(const std::vector<std::string>& args, const WindowPrefs& prefs) {
    WindowMode m;
    if (const auto o = arg_option(args, {"windowed", "window", "size"}, 2)) {
        m.size = {at_least(kMinCmdLineWidth, (*o)[0]), at_least(kMinCmdLineHeight, (*o)[1]), 60};
        m.overridden = true;
    } else if (const auto f = arg_option(args, {"fullscreen"}, 2)) {
        m.fullscreen = true;
        m.size = {at_least(kDefaultWidth, (*f)[0]), at_least(kDefaultHeight, (*f)[1]), 60};
        m.overridden = true;
    } else if (const auto r = parse_resolution(prefs.primary_adapter);
               r && !same_no_case(prefs.primary_adapter, "windowed")) {
        m.fullscreen = true;
        m.size = *r;
    } else {
        // "windowed" (or anything else): the size and place it last had
        m.size = {prefs.width.value_or(kDefaultWidth), prefs.height.value_or(kDefaultHeight), 60};
        if (prefs.x && prefs.y) m.position = std::array<i32, 2>{*prefs.x, *prefs.y};
        m.maximized = prefs.maximized;
    }
    if (!m.fullscreen) {
        if (arg_option(args, {"maximize", "maximized"}, 0)) m.maximized = true;
        if (const auto p = arg_option(args, {"position"}, 2)) {
            m.position = std::array<i32, 2>{static_cast<i32>(integer((*p)[0]).value_or(0)),
                                            static_cast<i32>(integer((*p)[1]).value_or(0))};
        }
    }
    return m;
}

std::string native_fullscreen_key(std::string_view primary_adapter) {
    if (parse_resolution(primary_adapter)) {
        return std::string(primary_adapter);
    }
    return std::string(kDefaultAdapterMode);
}

std::pair<std::vector<OptionState>, std::string>
native_adapter_states(std::string_view primary_adapter, bool overridden) {
    if (overridden) {
        return adapter_states({}, true);
    }
    std::vector<OptionState> states{{"<LOC OPTIONS_0070>Windowed", "windowed"},
                                    {"Full Screen", native_fullscreen_key(primary_adapter)}};
    return {std::move(states), std::string(kDefaultAdapterMode)};
}

std::string adapter_option_for(bool fullscreen, std::string_view primary_adapter) {
    return fullscreen ? native_fullscreen_key(primary_adapter) : std::string("windowed");
}

std::pair<std::vector<OptionState>, std::string>
adapter_states(const std::vector<Resolution>& modes, bool overridden) {
    std::vector<OptionState> states;
    if (overridden) {
        states.push_back({"<LOC _Command_Line_Override>", "overridden"});
        return {std::move(states), "overridden"};
    }
    states.push_back({"<LOC OPTIONS_0070>Windowed", "windowed"});
    std::set<std::tuple<u32, u32, u32>> listed;
    for (const Resolution& r : modes) {
        if (r.width < kDefaultWidth || r.height < kDefaultHeight) continue;
        if (!listed.insert({r.width, r.height, r.rate}).second) continue;
        states.push_back({std::to_string(r.width) + "x" + std::to_string(r.height) + "(" +
                              std::to_string(r.rate) + ")",
                          std::to_string(r.width) + "," + std::to_string(r.height) + "," +
                              std::to_string(r.rate)});
    }
    return {std::move(states), std::string(kDefaultAdapterMode)};
}

} // namespace osc::app
