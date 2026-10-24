#pragma once

// Lua registry keys shared by the engine's layers (core, ui, renderer, lua)
// for the UI state. Kept free of dependencies so any layer can include it.
namespace osc::core {

/// True while FA's game interface exists (set by ui::WldUIProvider). The
/// game UI's beat functions run only then, and the renderer draws the C++
/// HUD placeholders only when it does not exist (or with --legacy-hud).
inline constexpr const char* kWorldUiActiveKey = "__osc_world_ui_active";

/// True once RestartSession asked for the game in play to start again; the
/// window takes it between frames, as Moho's CreateSession frame action.
inline constexpr const char* kRestartRequestKey = "__osc_restart_requested";

} // namespace osc::core
