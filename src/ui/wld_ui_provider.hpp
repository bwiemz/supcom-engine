#pragma once

#include "core/types.hpp"

struct lua_State;

namespace osc::ui {

/// C++ backing for moho.WldUIProvider_methods.
/// FA's gamemain.lua calls CreateWldUIProvider() which creates this.
/// The provider manages the lifecycle of the in-game HUD.
class WldUIProvider {
public:
    WldUIProvider() = default;

    /// Called by gamemain.lua to construct the HUD.
    /// Triggers CreateGameInterface(isReplay) callback.
    void create_game_interface(lua_State* L, bool is_replay);

    /// Called when leaving game state — destroys HUD controls.
    void destroy_game_interface(lua_State* L);

    /// Loading screen progress (stub for now, formalized in M152).
    void start_loading_dialog(lua_State* L);
    void update_loading_dialog(lua_State* L, f32 progress);
    void stop_loading_dialog(lua_State* L);

    bool game_interface_created() const { return game_interface_created_; }

private:
    bool game_interface_created_ = false;
};

} // namespace osc::ui
