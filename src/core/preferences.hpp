#pragma once

#include <filesystem>
#include <string>
#include <string_view>

struct lua_State;

namespace osc::core {

/// FA's preferences (Game.prefs): one Lua table of plain data -- profiles,
/// options, window positions, filters -- read and written by dotted path,
/// e.g. GetPreference("profile.current"). As in Moho it lives in a private
/// Lua state; values cross into a script state as deep copies, so a table
/// a script gets back is its own until it calls SetPreference.
///
/// A path segment of digits indexes by number ("profile.profiles.1").
class Preferences {
public:
    Preferences();
    ~Preferences();
    Preferences(const Preferences&) = delete;
    Preferences& operator=(const Preferences&) = delete;

    /// Replace the content with a Game.prefs file: Lua source whose global
    /// assignments are the preferences. It runs with no globals, so it can
    /// hold data only. False (content unchanged) if missing or invalid.
    bool load(const std::filesystem::path& path);

    /// Write the content as Lua source, keys sorted (stable diffs). False
    /// on an I/O error. Functions and cyclic references are not written.
    bool save(const std::filesystem::path& path) const;

    /// The file SavePreferences() writes; empty keeps preferences in
    /// memory only (tests, captures).
    void set_path(std::filesystem::path path) { path_ = std::move(path); }
    const std::filesystem::path& path() const { return path_; }
    /// save(path()), if there is one.
    bool save() const { return !path_.empty() && save(path_); }

    /// Push onto L a copy of the value at `key`, or nil.
    void push(std::string_view key, lua_State* L) const;
    /// Set `key` to a copy of L's value at `idx`; nil removes it. Missing
    /// tables on the way are created (a non-table there is replaced).
    void set(std::string_view key, lua_State* L, int idx);

    /// The current profile's dotted path ("profile.profiles.N"), or empty
    /// when there is none: retail's Prefs.GetCurrentProfile.
    std::string current_profile_path() const;

    /// Push the current profile's option `key` (its options table when
    /// `key` is empty), or nil: Moho's GetOptions.
    void push_option(std::string_view key, lua_State* L) const;

    /// Make sure a current profile exists: with none, add {Name = name} and
    /// make it current. Retail asks for a profile before its menus work.
    /// Returns true if one was created.
    bool ensure_profile(std::string_view name);

    // Typed access for engine code; a missing or differently typed value
    // gives the default.
    std::string get_string(std::string_view key, const std::string& default_val) const;
    float get_float(std::string_view key, float default_val) const;
    bool get_bool(std::string_view key, bool default_val) const;
    int get_int(std::string_view key, int default_val) const;
    void set_string(std::string_view key, const std::string& val);
    void set_float(std::string_view key, float val);
    void set_bool(std::string_view key, bool val);
    void set_int(std::string_view key, int val);

private:
    /// Push the value at `key` in L_ (not a copy), or nil.
    void walk(std::string_view key) const;
    /// Set `key` to the value on top of L_ (popped).
    void assign(std::string_view key);

    lua_State* L_ = nullptr;
    int root_ref_ = -2; // LUA_NOREF
    std::filesystem::path path_;
};

} // namespace osc::core
