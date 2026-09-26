#pragma once

#include "core/types.hpp"

#include <functional>
#include <vector>
#include <string>
#include <iosfwd>

struct lua_State;

namespace osc::sim { class FrameView; class SimState; }
namespace osc::lua { class LuaState; }
namespace osc::vfs { class VirtualFileSystem; }
namespace osc::blueprints { class BlueprintStore; }

namespace osc::test {

/// Entity id of an army's commander (0-based army; lowest-id registered unit
/// with the COMMAND category, destroyed or not), or 0 if it has none. Tests
/// used to hardcode entity #1/#2, which only held when the ACUs happened to be
/// created before any prop (FAF); retail creates props and deposits first.
u32 army_acu_id(sim::SimState& sim, i32 army);

/// Lua helpers for the embedded test scripts, e.g. __osc_test_acu_id(army)
/// (1-based army, mirroring the Lua convention) -> entity id or nil.
void register_test_helpers(lua_State* L);

/// Dependencies shared across all integration tests.
struct TestContext {
    sim::SimState& sim;
    lua::LuaState& lua_state;
    lua_State* L;
    vfs::VirtualFileSystem& vfs;
    blueprints::BlueprintStore& store;
};

void test_damage(TestContext& ctx);
void test_move(TestContext& ctx);
void test_fire(TestContext& ctx);
void test_economy(TestContext& ctx);
void test_build(TestContext& ctx);
void test_chain(TestContext& ctx);
void test_ai(TestContext& ctx);
void test_reclaim(TestContext& ctx);
void test_threat(TestContext& ctx);
void test_combat(TestContext& ctx);
void test_platoon(TestContext& ctx);
void test_repair(TestContext& ctx);
void test_upgrade(TestContext& ctx);
void test_capture(TestContext& ctx);
void test_path(TestContext& ctx);
void test_toggle(TestContext& ctx);
void test_enhance(TestContext& ctx);
void test_intel(TestContext& ctx);
void test_shield(TestContext& ctx);
void test_transport(TestContext& ctx);
void test_fow(TestContext& ctx);
void test_los(TestContext& ctx);
void test_stall(TestContext& ctx);
void test_jammer(TestContext& ctx);
void test_stub(TestContext& ctx);
void test_audio(TestContext& ctx);
void test_bone(TestContext& ctx);
void test_manip(TestContext& ctx);
void test_canpath(TestContext& ctx);
void test_armor(TestContext& ctx);
void test_vet(TestContext& ctx);
void test_wreck(TestContext& ctx);
void test_adjacency(TestContext& ctx);
void test_stats(TestContext& ctx);
void test_silo(TestContext& ctx);
void test_flags(TestContext& ctx);
void test_layercap(TestContext& ctx);
void test_massstub(TestContext& ctx);
void test_massstub2(TestContext& ctx);
void test_massstub3(TestContext& ctx);
void test_anim(TestContext& ctx);
void test_teamcolor(TestContext& ctx);
void test_normal(TestContext& ctx);
void test_prop(TestContext& ctx);
void test_scale(TestContext& ctx);
void test_specular(TestContext& ctx);
void test_terrain_normal(TestContext& ctx);
void test_decal(TestContext& ctx);
void test_projectile(TestContext& ctx);
void test_weapon(TestContext& ctx);
void test_targeting(TestContext& ctx);
void test_aim(TestContext& ctx);
void test_death(TestContext& ctx);
void test_impact(TestContext& ctx);
void test_arc(TestContext& ctx);
void test_collide(TestContext& ctx);
void test_area(TestContext& ctx);
void test_drive(TestContext& ctx);
void test_crowd(TestContext& ctx);
void test_formation(TestContext& ctx);
void test_missile(TestContext& ctx);
void test_defence(TestContext& ctx);
void test_beam_weapon(TestContext& ctx);
void test_charge(TestContext& ctx);
void test_range(TestContext& ctx);
void test_ferry(TestContext& ctx);
void test_prebuilt(TestContext& ctx);
void test_transport_slots(TestContext& ctx);
void test_transport_pickup(TestContext& ctx);
void test_transport_drop(TestContext& ctx);
void test_carrier(TestContext& ctx);
void test_air_turn(TestContext& ctx);
void test_air_staging(TestContext& ctx);
void test_factory_assist(TestContext& ctx);
void test_factory_rally(TestContext& ctx);
void test_naval_depth(TestContext& ctx);
void test_influence(TestContext& ctx);
void test_issue_handles(TestContext& ctx);
void test_terrain_tex(TestContext& ctx);
void test_shadow(TestContext& ctx);
void test_massstub4(TestContext& ctx);
void test_spatial(TestContext& ctx);
void test_unitsound(TestContext& ctx);
void test_medstub(TestContext& ctx);
void test_lowstub(TestContext& ctx);
void test_blend(TestContext& ctx);
void test_ui(TestContext& ctx);
void test_bitmap(TestContext& ctx);
void test_text(TestContext& ctx);
void test_edit(TestContext& ctx);
void test_controls(TestContext& ctx);
void test_uiboot(TestContext& ctx);
void test_uirender(TestContext& ctx);
void test_font(TestContext& ctx);
void test_scissor(TestContext& ctx);
void test_border_render(TestContext& ctx);
void test_edit_render(TestContext& ctx);
void test_itemlist_render(TestContext& ctx);
void test_scrollbar_render(TestContext& ctx);
void test_anim_render(TestContext& ctx);
void test_tiled_render(TestContext& ctx);
void test_input(TestContext& ctx);
void test_onframe(TestContext& ctx);
/// Retail in-game UI (M187): the engine drove uimain.StartGameUI and the
/// provider's StartLoadingDialog -> CreateGameInterface -> StopLoadingDialog.
/// `pump_frames(n)` advances n UI frames (threads, OnFrame); `play(n)` plays
/// n sim ticks as the game loop does (tick, sim beat to the UI, UI frames).
/// `click(x, z, shift)` is a world left-click under FA's command mode (true
/// if it issued a command).
void test_gameui(TestContext& ctx, const std::function<void(int)>& pump_frames,
                 const std::function<void(int)>& play,
                 const std::function<bool(f32, f32, bool)>& click,
                 const std::function<bool(const char*)>& sim_lua);

/// --victory-test: retail's /lua/victory.lua decides a real game. Every
/// other army loses its commander; the script defeats them (OnDefeat ->
/// SetArmyOutOfGame and Sync.GameResult), declares the survivor's victory
/// after its 15 s hold, and ends the session (EndGame); the UI hears each
/// result (DoGameResult) and NoteGameOver, and the game is not paused.
void test_victory_flow(TestContext& ctx, const std::function<void(int)>& pump_frames,
                       const std::function<void(int)>& play,
                       const std::function<bool(const char*)>& sim_lua);

/// --interp-test: army 1's ACU walks while the windowed loop runs four
/// frames per sim tick. Drawn between ticks, its position must change on
/// (nearly) every frame -- stepping with the ticks would change it on one
/// frame in four -- and always lie between its positions at the last two
/// ticks.
class InterpProbe {
public:
    /// Once per frame, after the frame's ticks, with the view it draws.
    void on_frame(sim::SimState& sim, const sim::FrameView& view,
                  const std::function<bool(const char*)>& sim_lua);
    bool done() const { return done_; }

private:
    void finish();

    u32 acu_ = 0;
    int frames_ = 0;         // since the move order
    bool have_last_ = false;
    f32 last_[3] = {};
    int moving_frames_ = 0;  // the ACU moved between the last two ticks
    int changed_frames_ = 0; // ...and its drawn position changed this frame
    int off_segment_ = 0;    // drawn outside the two ticks' positions
    bool done_ = false;
};

/// --render-dump <file>: a scripted scene (army 1's tanks, a shield
/// generator and an engineer building, fighting army 2's bots, with army
/// 1's units selected) rendered offscreen at four frames per tick; every
/// 16th frame from tick 100 to 180 is written with Renderer::dump_frame.
/// Two runs give identical files, so a refactor of the render path can be
/// checked frame for frame against a dump taken before it.
class RenderDumpProbe {
public:
    explicit RenderDumpProbe(std::string path) : path_(std::move(path)) {}
    /// Once per frame, after render().
    void on_frame(sim::SimState& sim, const std::function<bool(const char*)>& sim_lua,
                  const std::function<void(const std::vector<u32>&)>& select,
                  const std::function<void(std::ostream&)>& dump);
    bool done() const { return done_; }

private:
    std::string path_;
    std::string text_;
    int frames_ = 0;
    bool scene_ = false;
    bool selected_ = false;
    bool done_ = false;
};

void test_cursor_render(TestContext& ctx);
void test_drag_render(TestContext& ctx);
void test_emitter(TestContext& ctx);
void test_collision_beam(TestContext& ctx);
void test_decal_splat(TestContext& ctx);
void test_commands(TestContext& ctx);
void test_deposits(TestContext& ctx);
void test_beams(TestContext& ctx);
void test_shield_render(TestContext& ctx);
void test_vet_adj_render(TestContext& ctx);
void test_intel_overlay(TestContext& ctx);
void test_enhance_wreck_render(TestContext& ctx);
void test_vfx_render(TestContext& ctx);
void test_transport_silo_render(TestContext& ctx);
void test_profile(TestContext& ctx);

} // namespace osc::test
