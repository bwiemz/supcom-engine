#pragma once

#include "core/types.hpp"

#include <array>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

struct lua_State;

namespace osc::ui {

/// Moho's CUIKeyHandler: the key names (by Windows virtual-key code, from
/// keyNames.lua), and one map from a key chord to its action, a console
/// command. A chord is the key's virtual-key code with modifier bits.
class KeyMapRegistry {
public:
    static constexpr u32 kShift = 0x80000000u;
    static constexpr u32 kCtrl = 0x40000000u;
    static constexpr u32 kAlt = 0x20000000u;

    /// Names start as "Unknown%02X", as Moho's do.
    KeyMapRegistry();

    /// Name keys from keyNames.lua's `keyNames` at `table_idx`: hex code
    /// strings to names (CUIKeyHandler::SetKeyNameTable).
    void set_key_names(lua_State* L, int table_idx);
    const std::string& key_name(u32 code) const { return names_[code & 0xFF]; }

    /// IN_ParseKeyModifiers: `Ctrl-Shift-A` to a chord. The last token is the
    /// key, the rest modifiers, all case-insensitive; an unknown key is -1
    /// (a chord no key makes), an empty spec 0.
    i32 parse(std::string_view spec) const;

    /// IN_AddKeyMapTable: each `key = {action = "...", keyRepeat = bool}`
    /// sets its chord's action (a later table overwrites an earlier one).
    void add(lua_State* L, int table_idx);
    /// IN_RemoveKeyMapTable: the table's chords are unbound.
    void remove(lua_State* L, int table_idx);
    void clear();

    /// The chord a key press makes (its virtual-key code and modifiers).
    static u32 chord(i32 glfw_key, i32 glfw_mods);
    /// A chord's action, or null if it has none.
    const std::string* action(u32 chord) const;
    /// Whether a chord acts on the key's auto-repeat too (keyRepeat).
    bool repeats(u32 chord) const { return repeats_.count(chord) != 0; }
    size_t size() const { return actions_.size(); }

private:
    std::array<std::string, 256> names_;
    std::unordered_map<u32, std::string> actions_;
    std::unordered_set<u32> repeats_;
};

} // namespace osc::ui
