#pragma once

#include "core/types.hpp"

struct lua_State;

namespace osc::ui {

/// C++ backing for moho.WldUIProvider_methods.
///
/// Moho's world-UI flow: the engine calls uimain.StartGameUI, which runs
/// gamemain.CreateWldUIProvider -> WldUIProvider() -> InternalCreateWldUIProvider.
/// gamemain overrides the provider's methods per instance; the engine then
/// calls them on that Lua object: StartLoadingDialog while the world loads,
/// CreateGameInterface(isReplay) once it is ready (gamemain.CreateUI),
/// StopLoadingDialog, and DestroyGameInterface when the session ends.
class WldUIProvider {
public:
    /// Registry key holding the current Lua provider object.
    static constexpr const char* kLuaObjectKey = "__osc_wld_ui_provider_obj";

    WldUIProvider() = default;

    /// Each returns false if there is no provider object or the call errored.
    bool create_game_interface(lua_State* L, bool is_replay);
    bool destroy_game_interface(lua_State* L);
    bool start_loading_dialog(lua_State* L);
    bool update_loading_dialog(lua_State* L, f32 elapsed_seconds);
    bool stop_loading_dialog(lua_State* L);

    bool game_interface_created() const { return game_interface_created_; }

private:
    /// obj:name(args...) on the Lua provider object. The nargs arguments are
    /// on top of the stack and are consumed either way.
    static bool call_method(lua_State* L, const char* name, int nargs);

    bool game_interface_created_ = false;
};

} // namespace osc::ui
