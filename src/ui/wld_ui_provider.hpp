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
/// StopLoadingDialog, OnStart once the world plays, and DestroyGameInterface
/// when the session ends.
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

    /// OnStart is due: the game's interface was made (create_game_interface
    /// on a provider) and the world hasn't started since.
    bool start_due() const { return start_due_; }
    /// The world plays: OnStart(self), once per game (Moho's
    /// CLuaWldUIProvider::OnStart). Moho calls it the frame after
    /// CreateGameInterface, as its frame action enters Playing with every
    /// client ready (WLD_DoPostInitializing, or WLD_DoWaiting after a wait),
    /// before that frame's first world frame. Does nothing unless due; false
    /// then, or with no OnStart (retail's provider defines none) or an error.
    bool start_game(lua_State* L);

private:
    /// obj:name(args...) on the Lua provider object. The nargs arguments are
    /// on top of the stack and are consumed either way.
    static bool call_method(lua_State* L, const char* name, int nargs);

    bool game_interface_created_ = false;
    bool start_due_ = false;
};

} // namespace osc::ui
