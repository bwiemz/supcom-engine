#pragma once

// The user state's stand-ins: the Moho-named globals FA's UI bootstrap
// expects that the engine does not implement yet. They are one table,
// registered once each, in one place (App::init_ui_state, after the UI
// state's real bindings). Each answer is one of a closed set of kinds, and
// none of them is nil: a function Moho answers with a value answers a value
// of that type here too. (A nil answer had broken the Options screen's
// fidelity presets and FAF's reticles.)

#include <concepts>
#include <cstddef>
#include <span>
#include <variant>

extern "C" {
#include <lua.h>
}

namespace osc::lua {

class LuaState;

/// A string a stand-in holds: a string literal, never a null pointer.
class StandInText {
public:
    template <std::size_t N> consteval StandInText(const char (&text)[N]) : text_(text) {}
    constexpr const char* c_str() const { return text_; }

private:
    const char* text_;
};

namespace stand_in {

/// A function that does nothing and answers nothing.
struct Command {};

/// A function that answers this boolean.
struct ReturnsBool {
    template <std::same_as<bool> B> consteval explicit ReturnsBool(B v) : value(v) {}
    bool value;
};

/// A global string, not a function.
struct GlobalString {
    StandInText value;
};

/// A global array of strings, not a function.
struct GlobalStrings {
    std::span<const StandInText> values;
};

} // namespace stand_in

/// What a stand-in answers. There is no nil kind.
using StandInAnswer = std::variant<stand_in::Command, stand_in::ReturnsBool, stand_in::GlobalString,
                                   stand_in::GlobalStrings>;

struct UserStandIn {
    consteval UserStandIn(StandInText global, const StandInAnswer& what)
        : name(global), answer(what) {}
    StandInText name;
    StandInAnswer answer;
};

/// The table, in the order it is registered.
std::span<const UserStandIn> user_stand_ins();

/// Define each stand-in in the UI state, replacing nothing: no real binding
/// has a stand-in's name (test_user_stand_ins checks the ones the boot
/// registers first).
void register_user_stand_ins(LuaState& state);

/// Whether `f` is the function every function stand-in is (its entry an
/// upvalue): a global bound to it is not implemented.
bool is_user_stand_in(lua_CFunction f);

} // namespace osc::lua
