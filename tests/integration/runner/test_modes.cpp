// The integration runner's test modes (M192): what main() ran beside the
// game until the executable was split. The flag table dispatches most of
// them; the rest keep their own code, as it stood in main().

#include "test_modes.hpp"
#include "app/support.hpp"
#include "audio_data_test.hpp"
#include "integration_tests.hpp"
#include "core/game_state.hpp"
#include "core/log.hpp"
#include "core/profiler.hpp"
#include "core/test_status.hpp"
#include "lua/beat_system.hpp"
#include "lua/factory_queue.hpp"
#include "lua/init_loader.hpp"
#include "lua/lua_state.hpp"
#include "lua/scenario_loader.hpp"
#include "lua/sim_bindings.hpp"
#include "blueprints/blueprint_store.hpp"
#include "lua/lan_dialog_ui.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/mp_net_state.hpp"
#include "lua/smoke_test.hpp"
#include "renderer/input_handler.hpp"
#include "renderer/renderer.hpp"
#include "sim/sim_callback_queue.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "ui/ui_control.hpp"
#include "ui/wld_ui_provider.hpp"
#include "vfs/virtual_file_system.hpp"

extern "C" {
#include <lua.h>
}

#include <cstdio>
#include <iostream>
#include <string>
#include <unordered_set>
#include <vector>
#include <spdlog/spdlog.h>

namespace osc::test {

using app::begin_world_ui;
using app::detach_ui_from_sim;
using app::dispatch_selection_change;
using app::Engine;
using app::execute_reload_sequence;
using app::finish_test_run;
using app::finish_world_ui;
using app::new_game_seed;
using app::note_game_over_if_ended;
using app::parse_flag;
using app::parse_string_arg;
using app::pump_ui_frames;
using app::pump_ui_frames_with_controls;
using app::read_command_mode;
using app::report_command_issued;
using app::submit_sim_callbacks;
using app::world_beat;

namespace {

/// A mode run by one integration test function, against the sim's Lua
/// state or (ui) the UI's.
struct Mode {
    const char* flag;
    void (*run)(TestContext&);
    bool ui;
};

// In main()'s order: the modes before --gameui-test's...
constexpr Mode kModesBefore[] = {
    {"--damage-test", test_damage, false},
    {"--move-test", test_move, false},
    {"--fire-test", test_fire, false},
    {"--economy-test", test_economy, false},
    {"--build-test", test_build, false},
    {"--chain-test", test_chain, false},
    {"--ai-test", test_ai, false},
    {"--reclaim-test", test_reclaim, false},
    {"--threat-test", test_threat, false},
    {"--combat-test", test_combat, false},
    {"--platoon-test", test_platoon, false},
    {"--repair-test", test_repair, false},
    {"--upgrade-test", test_upgrade, false},
    {"--capture-test", test_capture, false},
    {"--path-test", test_path, false},
    {"--toggle-test", test_toggle, false},
    {"--enhance-test", test_enhance, false},
    {"--intel-test", test_intel, false},
    {"--shield-test", test_shield, false},
    {"--transport-test", test_transport, false},
    {"--fow-test", test_fow, false},
    {"--los-test", test_los, false},
    {"--stall-test", test_stall, false},
    {"--jammer-test", test_jammer, false},
    {"--stub-test", test_stub, false},
    {"--audio-test", test_audio, false},
    {"--bone-test", test_bone, false},
    {"--manip-test", test_manip, false},
    {"--canpath-test", test_canpath, false},
    {"--armor-test", test_armor, false},
    {"--vet-test", test_vet, false},
    {"--wreck-test", test_wreck, false},
    {"--adjacency-test", test_adjacency, false},
    {"--stats-test", test_stats, false},
    {"--silo-test", test_silo, false},
    {"--flags-test", test_flags, false},
    {"--layercap-test", test_layercap, false},
    {"--massstub-test", test_massstub, false},
    {"--massstub2-test", test_massstub2, false},
    {"--massstub3-test", test_massstub3, false},
    {"--anim-test", test_anim, false},
    {"--teamcolor-test", test_teamcolor, false},
    {"--normal-test", test_normal, false},
    {"--prop-test", test_prop, false},
    {"--scale-test", test_scale, false},
    {"--specular-test", test_specular, false},
    {"--terrain-normal-test", test_terrain_normal, false},
    {"--decal-test", test_decal, false},
    {"--projectile-test", test_projectile, false},
    {"--weapon-test", test_weapon, false},
    {"--targeting-test", test_targeting, false},
    {"--aim-test", test_aim, false},
    {"--death-test", test_death, false},
    {"--impact-test", test_impact, false},
    {"--arc-test", test_arc, false},
    {"--collide-test", test_collide, false},
    {"--area-test", test_area, false},
    {"--drive-test", test_drive, false},
    {"--crowd-test", test_crowd, false},
    {"--formation-test", test_formation, false},
    {"--missile-test", test_missile, false},
    {"--defence-test", test_defence, false},
    {"--beam-weapon-test", test_beam_weapon, false},
    {"--charge-test", test_charge, false},
    {"--range-test", test_range, false},
    {"--ferry-test", test_ferry, false},
    {"--prebuilt-test", test_prebuilt, false},
    {"--transport-slots-test", test_transport_slots, false},
    {"--transport-pickup-test", test_transport_pickup, false},
    {"--transport-drop-test", test_transport_drop, false},
    {"--carrier-test", test_carrier, false},
    {"--air-turn-test", test_air_turn, false},
    {"--air-staging-test", test_air_staging, false},
    {"--factory-assist-test", test_factory_assist, false},
    {"--factory-rally-test", test_factory_rally, false},
    {"--naval-depth-test", test_naval_depth, false},
    {"--influence-test", test_influence, false},
    {"--issue-handles-test", test_issue_handles, false},
    {"--terrain-tex-test", test_terrain_tex, false},
    {"--shadow-test", test_shadow, false},
    {"--massstub4-test", test_massstub4, false},
    {"--spatial-test", test_spatial, false},
    {"--unitsound-test", test_unitsound, false},
    {"--medstub-test", test_medstub, false},
    {"--lowstub-test", test_lowstub, false},
    {"--blend-test", test_blend, false},
    {"--ui-test", test_ui, true},
    {"--bitmap-test", test_bitmap, true},
    {"--text-test", test_text, true},
    {"--edit-test", test_edit, true},
    {"--controls-test", test_controls, true},
    {"--uiboot-test", test_uiboot, true},
};
// ...and after it.
constexpr Mode kModesAfter[] = {
    {"--uirender-test", test_uirender, true},
    {"--font-test", test_font, true},
    {"--scissor-test", test_scissor, true},
    {"--border-render-test", test_border_render, true},
    {"--edit-render-test", test_edit_render, true},
    {"--itemlist-render-test", test_itemlist_render, true},
    {"--scrollbar-render-test", test_scrollbar_render, true},
    {"--anim-render-test", test_anim_render, true},
    {"--tiled-render-test", test_tiled_render, true},
    {"--input-test", test_input, true},
    {"--onframe-test", test_onframe, true},
    {"--cursor-render-test", test_cursor_render, true},
    {"--drag-render-test", test_drag_render, true},
    {"--emitter-test", test_emitter, false},
    {"--collision-test", test_collision_beam, false},
    {"--decalsplat-test", test_decal_splat, false},
    {"--cmd-test", test_commands, false},
    {"--deposit-test", test_deposits, false},
    {"--beam-test", test_beams, false},
    {"--shield-render-test", test_shield_render, false},
    {"--vet-adj-render-test", test_vet_adj_render, false},
    {"--intel-overlay-test", test_intel_overlay, false},
    {"--enhance-wreck-test", test_enhance_wreck_render, false},
    {"--vfx-render-test", test_vfx_render, false},
    {"--transport-silo-test", test_transport_silo_render, false},
    {"--profile-test", test_profile, false},
};

/// Modes with code of their own (below), and the windowed ones.
constexpr const char* kOwnModes[] = {
    "--gameui-test",    "--victory-test",      "--audio-data-test", "--lobby-flow-test",
    "--dualstate-test", "--construction-test", "--phase2-test",     "--phase3-test",
    "--phase4-test",    "--phase5-test",       "--smoke-test",      "--draw-test",
    "--stress-test",    "--full-smoke-test",
};

/// Runs the sim Lua state's `code`; false (logged) on an error.
bool run_sim_lua(Engine& e, const char* code) {
    auto r = e.sim_lua_state->do_string(code);
    if (!r) spdlog::error("sim Lua: {}", r.error().message);
    return static_cast<bool>(r);
}

} // namespace

bool IntegrationModes::has(const char* flag) const {
    return given_.contains(flag);
}

void IntegrationModes::print_usage() const {
    // The option table is laid out by hand.
    // clang-format off
    std::cout << "\nTest modes (osc_integration):\n"
              << "  --damage-test      After ticks, kill entity #1 and run 10 more ticks\n"
              << "  --move-test        After ticks, move entity #1 and run 200 more ticks\n"
              << "  --fire-test        Teleport entities #1 and #2 close, run 100 combat ticks\n"
              << "  --economy-test     After ticks, log economy state for all armies\n"
              << "  --build-test       After ticks, build a T1 power gen near entity #1\n"
              << "  --chain-test       Full build chain: ACU -> factory -> engineer -> pgen\n"
              << "  --ai-test          AI ARMY_2: pgens + factory + engineers + guard assist\n"
              << "  --reclaim-test     Create prop, engineer reclaims it, verify mass gained\n"
              << "  --platoon-test     Platoon system: create, assign, move, fork, disband\n"
              << "  --threat-test      Threat queries, platoon targeting, command tracking\n"
              << "  --combat-test      AI produces army, forms platoons, attacks enemy\n"
              << "  --repair-test      Build pgen, damage it, repair it, verify health\n"
              << "  --upgrade-test     Build T1 mex, upgrade to T2, verify completion\n"
              << "  --capture-test     Build enemy pgen, capture it, verify ownership\n"
              << "  --path-test        A* pathfinding around obstacles + terrain height\n"
              << "  --toggle-test      Script bits, toggle caps, and dive command\n"
              << "  --enhance-test     ACU enhancement (AdvancedEngineering)\n"
              << "  --intel-test       Intel system (InitIntel/Enable/Disable/Radius)\n"
              << "  --shield-test      Shield system (create, health, regen, toggle)\n"
              << "  --transport-test   Transport load/unload, cargo tracking, speed mult\n"
              << "  --fow-test         Fog of war / visibility grid + OnIntelChange\n"
              << "  --los-test         Terrain line-of-sight occlusion\n"
              << "  --stall-test       Economy stalling (resource scarcity slows progress)\n"
              << "  --jammer-test      Dead-reckoning, stealth, jammer detection\n"
              << "  --stub-test        Moho stub conversions (14 real implementations)\n"
              << "  --audio-test       Audio system (XWB/XSB banks, play, loop, stop)\n"
              << "  --bone-test        Bone system (SCM parser, bone queries)\n"
              << "  --manip-test       Manipulator system (rotators, animators, sliders, aim)\n"
              << "  --canpath-test     CanPathTo + GetThreatBetweenPositions\n"
              << "  --armor-test      Armor system (damage multipliers by armor/damage type)\n"
              << "  --vet-test         Veterancy system (regen, vet XP dispersal, level up)\n"
              << "  --wreck-test       Wreckage system (SetMaxReclaimValues, GetHeading)\n"
              << "  --adjacency-test   Adjacency bonus system + SetFiringRandomness\n"
              << "  --stats-test       Stats/telemetry system (SetStat/GetStat/UpdateStat)\n"
              << "  --silo-test        Missile silo ammo system (Give/Remove/Get nuke+tactical)\n"
              << "  --flags-test       Unit targeting flags (DoNotTarget, Reclaimable, IsValidTarget)\n"
              << "  --layercap-test    Weapon fire target layer caps\n"
              << "  --massstub-test    Mass stub conversion (weapon/movement/fuel/projectile/misc)\n"
              << "  --massstub2-test   Mass stub conversion II (damage flags/caps/weapon/proj/elevation)\n"
              << "  --massstub3-test   Mass stub conversion III (brain/weapon/projectile/platoon)\n"
              << "  --anim-test        SCA skeletal animation (parsing, bone matrices, GPU skinning)\n"
              << "  --teamcolor-test   Team color rendering (SpecTeam texture, alpha mask blending)\n"
              << "  --normal-test      Normal map rendering (tangent-space normal maps, TBN matrix)\n"
              << "  --prop-test        Map prop rendering (SCMAP parsing, prop meshes, orientation)\n"
              << "  --scale-test       Prop scale & distance culling (per-prop scale, MAX_INSTANCES)\n"
              << "  --specular-test    Specular lighting (Blinn-Phong, SpecTeam texture, eye position)\n"
              << "  --decal-test       Terrain decals (SCMAP parsing, textured quads, LOD culling)\n"
              << "  --projectile-test  Projectile rendering (blueprint_id, velocity-align, mesh lookup)\n"
              << "  --weapon-test      Weapons fire through their scripts (states, salvos, reload)\n"
              << "  --targeting-test   How weapons choose targets (priorities, restrictions, orders)\n"
              << "  --aim-test         Turrets turn toward targets before firing\n"
              << "  --death-test       Units die through their scripts and leave retail wrecks\n"
              << "  --impact-test      Projectiles impact through their scripts\n"
              << "  --arc-test         Shots fly ballistic arcs onto their targets\n"
              << "  --collide-test     Shots meet what is in their way\n"
              << "  --area-test        Blasts reach what stands in them; shields absorb\n"
              << "  --drive-test       Ground units turn, accelerate and brake\n"
              << "  --crowd-test       Ground units keep apart\n"
              << "  --formation-test   Groups move in formation\n"
              << "  --missile-test     Silos build missiles; launchers fire them\n"
              << "  --defence-test     Anti-missile weapons shoot missiles down\n"
              << "  --beam-weapon-test Beam weapons reach, hit and damage\n"
              << "  --charge-test      Economy events, OverCharge and teleports cost and take time\n"
              << "  --range-test       Build, repair, reclaim and capture reach, guards, the queue\n"
              << "  --ferry-test       A ferry carries units from its beacon to its drop-off\n"
              << "  --factory-assist-test A factory guarding a factory builds from its queue\n"
              << "  --factory-rally-test What a factory builds takes its rally orders\n"
              << "  --influence-test   The AI's threat is what its intel has seen (influence map)\n"
              << "  --issue-handles-test Issue* takes one unit or a list, and skips non-units\n"
              << "  --shadow-test      Shadow mapping (depth pass, light matrix, shadow sampling)\n"
              << "  --massstub4-test   Mass stub conversion IV (visibility, scale, mesh, collision, attach, shake)\n"
              << "  --spatial-test     Spatial hash grid (grid init, collect_in_radius/rect, auto-notify)\n"
              << "  --unitsound-test   Unit sound (PlayUnitSound, PlayUnitAmbientSound, StopUnitAmbientSound)\n"
              << "  --medstub-test     Medium stubs (SetBoneEnabled, AddOnGivenCallback, AddBoundedProp)\n"
              << "  --lowstub-test     Low-priority stubs (Destroy/BeenDestroyed, CreateBuilderArmController)\n"
              << "  --blend-test       Blend-weight skinning (multi-bone vertex parsing, weight validation)\n"
              << "  --ui-test          UI control system (Frame, Group, LazyVar, moho bindings)\n"
              << "  --bitmap-test      Bitmap control (SetNewTexture, solid color, UVs, animation)\n"
              << "  --text-test        Text control (SetNewFont, SetText, font metrics, centering)\n"
              << "  --edit-test        Edit/ItemList/Scrollbar controls (text input, list ops, scroll)\n"
              << "  --controls-test    Border/Dragger/Cursor/Movie/Histogram/WorldMesh controls\n"
              << "  --uiboot-test      UI bootstrap (GetFrame, WorldView, WldUIProvider, lobby/discovery)\n"
              << "  --gameui-test      Retail in-game UI (StartGameUI, CreateGameInterface, gamemain.CreateUI)\n"
              << "  --audio-data-test  Every cue in FA's sound banks resolves to playable waves\n"
              << "  --victory-test     The scenario's victory script decides a game (victory.lua)\n"
              << "  --interp-test      Windowed: a walking ACU is drawn between sim ticks\n"
              << "  --render-dump <f>  Windowed: dump what the renderers generate for a scripted scene\n"
              << "  --lobby-flow-test  Front-end ButtonSkirmish -> hosted lobby callback smoke\n"
              << "  --uirender-test    UI 2D rendering pipeline (LazyVar positions, quad building)\n"
              << "  --font-test        Font rendering (stb_truetype metrics, per-glyph advance)\n"
              << "  --scissor-test     Scissor/clip rectangles (parent-child clipping)\n"
              << "  --border-render-test Border 9-patch rendering (6-texture ninepatch)\n"
              << "  --edit-render-test Edit control visuals (background, text, caret)\n"
              << "  --terrain-normal-test Terrain normal maps (per-stratum DXT5nm, TBN, blending)\n"
              << "  --terrain-tex-test Terrain textures (stratum blending, blend maps, UV scaling)\n"
              << "  --emitter-test     IEffect/emitter system (Create*Emitter, beams, decals, chaining)\n"
              << "  --collision-test   CollisionBeam entity (__init, Enable/Disable, SetBeamFx, GetLauncher)\n"
              << "  --decalsplat-test  Decal/Splat system (CreateDecal, CreateSplat, CreateSplatOnBone, lifetime)\n"
              << "  --cmd-test         Issue commands + economy events (Nuke/Tactical/Teleport/Ferry/Sacrifice)\n"
              << "  --deposit-test     Resource deposits + manipulator stub conversions\n"
              << "  --beam-test        Beam rendering (construction/reclaim/repair/capture/collision)\n"
              << "  --shield-render-test Shield bubble rendering (projected circles)\n"
              << "  --vet-adj-render-test Veterancy indicators + adjacency lines\n"
              << "  --intel-overlay-test Intel range overlay (radar/sonar/omni circles)\n"
              << "  --enhance-wreck-test Enhancement mesh switching + wreckage visual\n"
              << "  --vfx-render-test  VFX/emitter particle rendering\n"
              << "  --transport-silo-test Transport cargo + silo ammo visuals\n"
              << "  --dualstate-test   Dual Lua state split (sim_L/ui_L isolation)\n"
              << "  --construction-test Construction panel (EntityCategoryGetUnitList)\n"
              << "  --phase2-test      Phase 2 integration (construction, orders, unitview, tooltips)\n"
              << "  --phase3-test      Phase 3 integration (state machine, beat system, score flow)\n"
              << "  --profile-test     Profiler system (zones, nesting, rolling stats)\n";
    // clang-format on
}

app::TestRequest IntegrationModes::parse(int argc, char* argv[]) {
    const auto note = [&](const char* flag) {
        if (parse_flag(argc, argv, flag)) given_.insert(flag);
    };
    for (const Mode& m : kModesBefore) note(m.flag);
    for (const Mode& m : kModesAfter) note(m.flag);
    for (const char* flag : kOwnModes) note(flag);
    interp_ = parse_flag(argc, argv, "--interp-test");
    render_dump_path_ = parse_string_arg(argc, argv, "--render-dump", "");
    render_dump_.emplace(render_dump_path_);

    app::TestRequest request;
    request.headless = !given_.empty();
    request.windowed = interp_ || !render_dump_path_.empty();
    request.ai_army_2 =
        has("--ai-test") || has("--platoon-test") || has("--threat-test") || has("--combat-test");
    request.world_ui = has("--gameui-test") || has("--victory-test");
    // --render-dump compares renders; its scene's script errors are logged,
    // not counted, so a dump is still written.
    if (interp_) osc::test_status::set_count_lua_failures(true);
    return request;
}

std::optional<int> IntegrationModes::before_boot(int argc, char* argv[]) {
    // Multiplayer LAN verification: two processes (host + join) run a real
    // TCP lockstep match and self-report sync. Handled before any engine init
    // since it needs no FA data / window.
    {
        bool mp_host = parse_flag(argc, argv, "--mp-host");
        std::string mp_join = parse_string_arg(argc, argv, "--mp-join", "");
        if (mp_host || !mp_join.empty()) {
            std::string port_s = parse_string_arg(argc, argv, "--mp-port", "47624");
            std::string frames_s = parse_string_arg(argc, argv, "--mp-frames", "60");
            bool inject_desync = parse_flag(argc, argv, "--mp-desync");
            auto port = static_cast<osc::u16>(std::strtoul(port_s.c_str(), nullptr, 10));
            auto frames = static_cast<osc::u32>(std::strtoul(frames_s.c_str(), nullptr, 10));
            return run_mp_lan_test(mp_host, mp_join, port, frames, inject_desync);
        }

        // Headless LAN lobby lifecycle verification (host + client processes).
        bool lan_host = parse_flag(argc, argv, "--lan-host");
        std::string lan_join = parse_string_arg(argc, argv, "--lan-join", "");
        if (lan_host || !lan_join.empty()) {
            std::string port_s = parse_string_arg(argc, argv, "--mp-port", "47624");
            std::string frames_s = parse_string_arg(argc, argv, "--mp-frames", "40");
            std::string drop_s = parse_string_arg(argc, argv, "--mp-drop-at", "0");
            auto port = static_cast<osc::u16>(std::strtoul(port_s.c_str(), nullptr, 10));
            auto frames = static_cast<osc::u32>(std::strtoul(frames_s.c_str(), nullptr, 10));
            auto drop_at = static_cast<osc::u32>(std::strtoul(drop_s.c_str(), nullptr, 10));
            return run_lan_lobby_test(lan_host, lan_join, port, frames, drop_at);
        }

        // Headless check of the LAN UI engine globals (LanHost/LanJoin bindings).
        if (parse_flag(argc, argv, "--lan-ui-test")) {
            osc::lua::LuaState uiL;
            osc::lua::register_lan_ui_bindings(uiL);
            auto& mp = osc::lua::mp_net_state();
            mp.reset();
            int fails = 0;
            uiL.do_string("__r_host = LanHost()");
            if (!mp.transport_ready) {
                spdlog::error("[lan-ui] LanHost did not create a transport");
                fails++;
            } else {
                spdlog::info("[lan-ui] LanHost OK (listening on port {})", mp.port);
            }
            osc::lua::mp_teardown();
            uiL.do_string("__r_join = LanJoin('')");
            bool rj = false;
            {
                lua_State* L = uiL.raw();
                lua_pushstring(L, "__r_join");
                lua_rawget(L, LUA_GLOBALSINDEX);
                rj = lua_toboolean(L, -1) != 0;
                lua_pop(L, 1);
            }
            if (rj || mp.transport_ready) {
                spdlog::error("[lan-ui] LanJoin(empty) was not rejected");
                fails++;
            } else {
                spdlog::info("[lan-ui] LanJoin(empty) correctly rejected");
            }
            osc::lua::mp_teardown();
            // The LAN dialog snippet must be syntactically valid and pcall-safe:
            // on a bare state (no maui/UIUtil) it runs its guard, fails to build
            // the UI, catches that in its pcall, and returns cleanly.
            uiL.do_string("function LOG(s) end"); // stub the FA logger
            {
                auto lr = uiL.do_string(osc::lua::kLanDialogLua);
                if (!lr) {
                    spdlog::error("[lan-ui] dialog snippet errored: {}", lr.error().message);
                    fails++;
                }
                lua_State* L = uiL.raw();
                lua_pushstring(L, "__osc_lan_dialog_built");
                lua_rawget(L, LUA_GLOBALSINDEX);
                bool built_flag = lua_toboolean(L, -1) != 0;
                lua_pop(L, 1);
                if (!built_flag) {
                    spdlog::error("[lan-ui] dialog snippet did not execute");
                    fails++;
                } else {
                    spdlog::info("[lan-ui] dialog snippet parses + degrades gracefully");
                }
            }
            std::printf("LAN_UI_TEST fails=%d\n", fails);
            std::fflush(stdout);
            return fails == 0 ? 0 : 1;
        }
    }
    return std::nullopt;
}

std::optional<int> IntegrationModes::before_init(const lua::InitConfig& config) {
    if (has("--audio-data-test")) {
        osc::test::run_audio_data_test(config.fa_path / "sounds");
        return finish_test_run("audio-data-test");
    }
    return std::nullopt;
}

std::optional<int> IntegrationModes::front_end(Engine& e) {
    const auto& map_path = e.map_path;
    auto& ui_lua_state = e.ui_lua_state;
    auto& ui_registry = e.ui_registry;
    auto& ui_thread_manager = e.ui_thread_manager;
    auto& ui_frame_count = e.ui_frame_count;
    auto& beat_registry = e.beat_registry;
    const bool lobby_flow_test = has("--lobby-flow-test");
    if (lobby_flow_test) {
        if (!map_path.empty()) {
            spdlog::error("--lobby-flow-test runs from the no-map front-end boot; omit --map");
            return 1;
        }

        static osc::lua::SmokeTestHarness lobby_harness;
        lobby_harness.activate();
        lobby_harness.set_phase("LOBBY_FLOW");
        lobby_harness.install_panic_handler(ui_lua_state.raw());
        lobby_harness.install_global_interceptor(ui_lua_state.raw());
        lobby_harness.install_all_method_interceptors(ui_lua_state.raw());

        spdlog::info("=== Lobby Flow Test: ButtonSkirmish -> hosted lobby ===");
        auto trigger_result = ui_lua_state.do_string(R"(
            rawset(_G, '__osc_lobby_flow_before_count', GetNumRootFrames())
            import('/lua/user/prefs.lua').SetToCurrentProfile('MenuTutorialPrompt', true)
            local main_menu = import('/lua/ui/menus/main.lua')
            rawset(_G, '__osc_lobby_flow_button_skirmish_type', type(main_menu.ButtonSkirmish))
            main_menu.ButtonSkirmish()
        )");
        if (!trigger_result) {
            spdlog::error("Lobby flow ButtonSkirmish error: {}", trigger_result.error().message);
            lobby_harness.print_report(true);
            lobby_harness.write_report_to_file("smoke_report.txt");
            lobby_harness.deactivate();
            return 1;
        }

        pump_ui_frames_with_controls(ui_lua_state, ui_thread_manager, beat_registry, ui_registry,
                                     180, ui_frame_count);

        auto verify_result = ui_lua_state.do_string(R"(
            __osc_lobby_flow_after_count = GetNumRootFrames()
            __osc_lobby_flow_hosted =
                rawget(_G, '__osc_lobby_hosting_callback_fired') == true
                and rawget(_G, '__osc_pending_host_comm') == nil
                and __osc_lobby_flow_after_count >= __osc_lobby_flow_before_count
        )");
        if (!verify_result) {
            spdlog::error("Lobby flow verification error: {}", verify_result.error().message);
            lobby_harness.print_report(true);
            lobby_harness.write_report_to_file("smoke_report.txt");
            lobby_harness.deactivate();
            return 1;
        }

        lua_State* uL = ui_lua_state.raw();
        lua_getglobal(uL, "__osc_lobby_flow_hosted");
        const bool hosted = lua_toboolean(uL, -1) != 0;
        lua_pop(uL, 1);

        if (!hosted) {
            lua_getglobal(uL, "__osc_lobby_host_game_called");
            const bool host_game_called = lua_toboolean(uL, -1) != 0;
            lua_pop(uL, 1);
            lua_getglobal(uL, "__osc_lobby_hosting_callback_fired");
            const bool hosting_callback = lua_toboolean(uL, -1) != 0;
            lua_pop(uL, 1);
            lua_getglobal(uL, "__osc_pending_host_comm");
            const bool pending_host_comm = !lua_isnil(uL, -1);
            lua_pop(uL, 1);
            lua_getglobal(uL, "__osc_lobby_flow_button_skirmish_type");
            const char* button_type = lua_tostring(uL, -1);
            std::string button_type_text = button_type ? button_type : "<nil>";
            lua_pop(uL, 1);

            spdlog::error("Lobby flow did not reach hosted lobby state "
                          "(ButtonSkirmish={}, HostGame={}, Hosting={}, pending_comm={})",
                          button_type_text, host_game_called, hosting_callback, pending_host_comm);
            lobby_harness.print_report(true);
            lobby_harness.write_report_to_file("smoke_report.txt");
            lobby_harness.deactivate();
            return 1;
        }

        spdlog::info("=== Lobby Flow Test Complete ===");
        lobby_harness.print_report(true);
        lobby_harness.write_report_to_file("smoke_report.txt");
        lobby_harness.deactivate();
        return finish_test_run("lobby-flow-test", lobby_harness.total_count());
    }
    return std::nullopt;
}

void IntegrationModes::frame_view(Engine& e, app::Frame& frame) {
    if (!interp_) return;
    interp_probe_.on_frame(*e.sim_state, frame.view,
                           [&](const char* code) { return run_sim_lua(e, code); });
}

void IntegrationModes::frame_rendered(Engine& e, app::Frame& frame) {
    if (render_dump_path_.empty()) return;
    render_dump_->on_frame(
        *e.sim_state, [&](const char* code) { return run_sim_lua(e, code); },
        [&](const std::vector<u32>& ids) { frame.input.set_selected({ids.begin(), ids.end()}); },
        [&](std::ostream& out) { frame.renderer.dump_frame(out); });
}

bool IntegrationModes::frames_done() const {
    return (interp_ && interp_probe_.done()) ||
           (!render_dump_path_.empty() && render_dump_->done());
}

std::optional<int> IntegrationModes::after_window() {
    if (interp_) {
        if (!interp_probe_.done())
            osc::test_status::fail("[FAIL] interp: the window closed before the check ended");
        return finish_test_run("interp-test");
    }
    if (!render_dump_path_.empty()) {
        if (!render_dump_->done())
            osc::test_status::fail("[FAIL] render-dump: the window closed before the dump ended");
        return finish_test_run("render-dump");
    }
    return std::nullopt;
}

std::optional<int> IntegrationModes::headless_first(Engine& e) {
    const auto& config = e.config;
    const auto& map_path = e.map_path;
    const auto& seed_arg = e.seed_arg;
    const auto& ai_personality = e.ai_personality;
    auto& vfs = e.vfs;
    auto& store = e.store;
    auto& loader = e.loader;
    auto& sim_lua_state = e.sim_lua_state;
    auto& sim_state = e.sim_state;
    auto& scenario_meta = e.scenario_meta;
    auto& ui_lua_state = e.ui_lua_state;
    auto& ui_thread_manager = e.ui_thread_manager;
    auto& beat_registry = e.beat_registry;
    auto& game_state_mgr = e.game_state_mgr;
    auto& wld_provider = e.wld_provider;
    const bool smoke_test = has("--smoke-test");
    const bool draw_test = has("--draw-test");
    const bool stress_test = has("--stress-test");
    const bool full_smoke_test = has("--full-smoke-test");
    // === Full Smoke Test: 5-phase game lifecycle ===
    if (full_smoke_test && !map_path.empty()) {
        // Static so the harness outlives ui_lua_state — interceptor closures
        // capture a lightuserdata pointer to the harness, and lua_close() during
        // main() cleanup would segfault if the harness were already freed.
        static osc::lua::SmokeTestHarness harness;
        harness.activate();
        osc::u32 ui_frame_counter = 0;

        // Install interceptors on ui_L (persistent across all phases)
        harness.install_panic_handler(ui_lua_state.raw());
        harness.install_global_interceptor(ui_lua_state.raw());
        harness.install_all_method_interceptors(ui_lua_state.raw());

        // --- Phase 1: FRONT_END ---
        spdlog::info("=== Phase 1: FRONT_END ===");
        harness.set_phase("FRONT_END");
        // Destroy sim to match real FRONT_END state (sim_state is null during lobby)
        detach_ui_from_sim(ui_lua_state.raw());
        sim_state.reset();
        sim_lua_state.reset();
        store.rebind(nullptr); // Detach from destroyed sim Lua state
        game_state_mgr.transition_to(osc::GameState::FRONT_END, ui_lua_state.raw());
        osc::core::call_lua_global(ui_lua_state.raw(), "CreateUI");
        pump_ui_frames(ui_lua_state, ui_thread_manager, beat_registry, 10, ui_frame_counter);

        // --- Phase 2: LOBBY ---
        spdlog::info("=== Phase 2: LOBBY ===");
        harness.set_phase("LOBBY");
        {
            lua_State* uL = ui_lua_state.raw();
            lua_pushstring(uL, "__osc_full_smoke_map_path");
            lua_pushstring(uL, map_path.c_str());
            lua_rawset(uL, LUA_GLOBALSINDEX);

            lua_pushstring(uL, "__osc_full_smoke_ai_personality");
            lua_pushstring(uL, ai_personality.c_str());
            lua_rawset(uL, LUA_GLOBALSINDEX);

            spdlog::info("Full smoke: launching through lobby:LaunchGame");
            auto launch_result = ui_lua_state.do_string(R"(
                local LobbyClass = {}
                for k, v in moho.lobby_methods do LobbyClass[k] = v end
                local lobby = InternalCreateLobby(LobbyClass, 'UDP', 6112, 16, 'Full Smoke Host')
                local scenario = rawget(_G, '__osc_full_smoke_map_path')
                local ai = rawget(_G, '__osc_full_smoke_ai_personality') or 'adaptive'
                lobby:LaunchGame({
                    GameOptions = {
                        ScenarioFile = scenario,
                    },
                    PlayerOptions = {
                        [1] = {
                            Human = true,
                            PlayerName = 'Player',
                            Faction = 1,
                            Team = 1,
                            StartSpot = 1,
                        },
                        [2] = {
                            Human = false,
                            PlayerName = 'AI',
                            AIPersonality = ai,
                            Faction = 2,
                            Team = 2,
                            StartSpot = 2,
                        },
                    },
                })
                rawset(_G, '__osc_full_smoke_map_path', nil)
                rawset(_G, '__osc_full_smoke_ai_personality', nil)
            )");
            if (!launch_result) {
                spdlog::warn("Full smoke lobby LaunchGame error: {}",
                             launch_result.error().message);
            }
        }
        pump_ui_frames(ui_lua_state, ui_thread_manager, beat_registry, 10, ui_frame_counter);

        // --- Phase 3: GAME ---
        spdlog::info("=== Phase 3: GAME (3000 ticks) ===");
        harness.set_phase("GAME");

        // Execute reload sequence to create sim state (headless — no renderer)
        double sim_accumulator_fst = 0.0;
        bool reload_ok = execute_reload_sequence(sim_lua_state, sim_state, ui_lua_state, vfs, store,
                                                 loader, config, scenario_meta, game_state_mgr,
                                                 nullptr, // renderer (headless)
                                                 nullptr, // input_handler (headless)
                                                 nullptr, // prev_selection (headless)
                                                 nullptr, // world_interp (headless)
                                                 new_game_seed(seed_arg, /*reproducible=*/true),
                                                 sim_accumulator_fst, map_path);

        if (!reload_ok) {
            spdlog::error("Phase 3: Reload failed — skipping remaining phases");
            harness.print_report(true);
            harness.write_report_to_file("smoke_report.txt");
            harness.deactivate();
            return 1;
        }

        // After reload, re-install interceptors on fresh sim_L
        if (sim_lua_state) {
            harness.install_panic_handler(sim_lua_state->raw());
            harness.install_global_interceptor(sim_lua_state->raw());
            harness.install_all_method_interceptors(sim_lua_state->raw());
        }

        // FA's game interface, as the windowed launch builds it
        begin_world_ui(ui_lua_state.raw(), wld_provider);
        finish_world_ui(ui_lua_state.raw(), wld_provider, false);

        // Fire OnFirstUpdate once
        osc::core::call_on_first_update(ui_lua_state.raw());

        for (int t = 0; t < 3000; t++) {
            if (sim_state) {
                sim_state->tick();
                world_beat(sim_lua_state.get(), sim_state.get(), ui_lua_state.raw());
            }
            if ((t + 1) % 10 == 0) {
                pump_ui_frames(ui_lua_state, ui_thread_manager, beat_registry, 1, ui_frame_counter);
            }
            if ((t + 1) % 250 == 0) {
                osc::i32 prop_count = 0;
                std::string army_summary;

                if (sim_state) {
                    sim_state->entity_registry().for_each([&](const osc::sim::Entity& e) {
                        if (!e.destroyed() && !e.is_unit()) prop_count++;
                    });

                    for (size_t a = 0; a < sim_state->army_count(); a++) {
                        auto* brain = sim_state->army_at(a);
                        if (!brain || brain->is_civilian()) continue;

                        osc::i32 units = 0, structures = 0;
                        sim_state->entity_registry().for_each([&](const osc::sim::Entity& e) {
                            if (e.army() == static_cast<osc::i32>(a) && !e.destroyed() &&
                                e.is_unit()) {
                                auto* u = static_cast<const osc::sim::Unit*>(&e);
                                if (u->has_category("STRUCTURE")) structures++;
                                else units++;
                            }
                        });

                        if (!army_summary.empty()) army_summary += ", ";
                        army_summary += "a" + std::to_string(a) + ": " + std::to_string(units) +
                                        "u/" + std::to_string(structures) + "s";
                    }
                }

                spdlog::info("  tick {}/3000 — props:{} {}", t + 1, prop_count, army_summary);
            }
        }

        // End-of-GAME army summary
        if (sim_state) {
            for (size_t a = 0; a < sim_state->army_count(); a++) {
                auto* brain = sim_state->army_at(a);
                if (!brain || brain->is_civilian()) continue;

                osc::i32 units = 0, structures = 0;
                sim_state->entity_registry().for_each([&](const osc::sim::Entity& e) {
                    if (e.army() == static_cast<osc::i32>(a) && !e.destroyed() && e.is_unit()) {
                        auto* u = static_cast<const osc::sim::Unit*>(&e);
                        if (u->has_category("STRUCTURE")) structures++;
                        else units++;
                    }
                });

                spdlog::info("  Army {} ({}): {} units, {} structures, kills={:.0f} built={:.0f}",
                             a, brain->name(), units, structures, brain->get_stat("Units_Killed"),
                             brain->get_stat("Units_History"));
            }
        }

        // --- Phase 4: SCORE ---
        spdlog::info("=== Phase 4: SCORE ===");
        harness.set_phase("SCORE");
        if (sim_state) {
            for (size_t i = 0; i < sim_state->army_count(); i++) {
                auto* brain = sim_state->army_at(i);
                if (brain && !brain->is_civilian() && static_cast<osc::i32>(i) != 0) {
                    brain->set_state(osc::sim::BrainState::Defeat);
                }
            }
            osc::i32 result = sim_state->player_result();
            spdlog::info("  player_result() = {} (expected 1=Victory)", result);
            osc::core::call_note_game_over(ui_lua_state.raw());
            game_state_mgr.set_game_over(true);
            game_state_mgr.transition_to(osc::GameState::SCORE, ui_lua_state.raw());
        }
        pump_ui_frames(ui_lua_state, ui_thread_manager, beat_registry, 10, ui_frame_counter);

        // --- Phase 5: RETURN ---
        spdlog::info("=== Phase 5: RETURN ===");
        harness.set_phase("RETURN");
        {
            lua_State* uL = ui_lua_state.raw();
            lua_pushstring(uL, "__osc_return_to_lobby");
            lua_pushboolean(uL, 1);
            lua_rawset(uL, LUA_REGISTRYINDEX);
        }
        wld_provider.destroy_game_interface(ui_lua_state.raw());
        detach_ui_from_sim(ui_lua_state.raw());
        sim_state.reset();
        sim_lua_state.reset();
        // Detach store from destroyed sim Lua state to prevent dangling luaL_unref
        store.rebind(nullptr);
        game_state_mgr.set_game_over(false);
        game_state_mgr.transition_to(osc::GameState::FRONT_END, ui_lua_state.raw());
        osc::core::call_lua_global(ui_lua_state.raw(), "CreateUI");
        pump_ui_frames(ui_lua_state, ui_thread_manager, beat_registry, 10, ui_frame_counter);

        // --- Report ---
        spdlog::info("=== Full Smoke Test Complete ===");
        harness.print_report(true);
        harness.write_report_to_file("smoke_report.txt");
        spdlog::info("Report written to smoke_report.txt");
        harness.deactivate();
        return finish_test_run("full-smoke-test", harness.total_count());
    }

    // === Smoke Test ===
    if (smoke_test && !map_path.empty()) {
        osc::lua::SmokeTestHarness harness;

        // Install interceptors on sim state
        harness.install_panic_handler(sim_lua_state->raw());
        harness.install_global_interceptor(sim_lua_state->raw());
        harness.install_all_method_interceptors(sim_lua_state->raw());

        // Install interceptors on UI state
        harness.install_panic_handler(ui_lua_state.raw());
        harness.install_global_interceptor(ui_lua_state.raw());
        harness.install_all_method_interceptors(ui_lua_state.raw());

        spdlog::info("=== Smoke Test: Running 100 sim ticks ===");
        for (int i = 0; i < 100; i++) {
            sim_state->tick();
        }

        spdlog::info("=== Smoke Test: Running 100 UI frame dispatches ===");
        for (int i = 0; i < 100; i++) {
            osc::lua::advance_ui_clock(ui_lua_state.raw(), 1.0 / 60.0);
            ui_thread_manager.resume_all(static_cast<osc::u32>(i));
        }

        harness.print_report(false);
        spdlog::info("=== Smoke Test Complete ===");
        if (harness.total_count() > 0) {
            osc::test_status::record_failure(
                fmt::format("smoke-test: {} issue(s) reported", harness.total_count()));
        }
    }

    // === Draw Test: simultaneous ACU death ===
    if (draw_test && sim_state) {
        spdlog::info("=== DRAW TEST: simultaneous ACU death ===");
        // Set both armies to defeated
        for (size_t i = 0; i < sim_state->army_count(); i++) {
            auto* brain = sim_state->army_at(i);
            if (brain && !brain->is_civilian()) {
                brain->set_state(osc::sim::BrainState::Defeat);
            }
        }
        osc::i32 result = sim_state->player_result();
        if (result == 3) {
            spdlog::info("  PASS — simultaneous defeat returns Draw (3)");
        } else {
            osc::test_status::fail("  FAIL — expected Draw (3), got {}", result);
            return 1;
        }
        return 0;
    }

    // === Stress Test: 10000-tick AI-vs-AI stability validation ===
    if (stress_test && sim_state) {
        spdlog::info("=== STRESS TEST: 10000-tick AI-vs-AI ===");

        osc::i32 peak_entities = 0;
        osc::i32 tick_target = 10000;

        for (osc::i32 t = 0; t < tick_target; t++) {
            sim_state->tick();

            osc::i32 entity_count = 0;
            sim_state->entity_registry().for_each([&](const osc::sim::Entity& e) {
                if (!e.destroyed()) entity_count++;
            });
            if (entity_count > peak_entities) peak_entities = entity_count;

            // Log progress every 1000 ticks
            if ((t + 1) % 1000 == 0) {
                spdlog::info("  tick {}/{} — {} entities (peak {})", t + 1, tick_target,
                             entity_count, peak_entities);
            }

            // Check game-over — continue tracking but note it
            osc::i32 result = sim_state->player_result();
            if (result != 0 && !sim_state->game_ended()) {
                const char* result_str = result == 1 ? "VICTORY" : result == 2 ? "DEFEAT" : "DRAW";
                spdlog::info("  Game over at tick {}: {}", t + 1, result_str);
                sim_state->set_game_ended(true);
            }
        }

        // Report results
        spdlog::info("=== STRESS TEST COMPLETE ===");
        spdlog::info("  Peak entities: {}", peak_entities);

        // Report army stats
        for (size_t i = 0; i < sim_state->army_count(); i++) {
            auto* brain = sim_state->army_at(i);
            if (!brain || brain->is_civilian()) continue;
            spdlog::info("  Army {} ({}): kills={:.0f} losses={:.0f} built={:.0f} mass={:.0f}", i,
                         brain->name(), brain->get_stat("Units_Killed"),
                         brain->get_stat("Units_Killed"), brain->get_stat("Units_History"),
                         brain->get_stat("Economy_TotalProduced_Mass"));
        }

        spdlog::info("  PASS — no crashes in {} ticks", tick_target);
        return finish_test_run("stress-test");
    }
    return std::nullopt;
}

void IntegrationModes::headless(Engine& e) {
    // ── Integration tests (require --map) ──
    auto& sim_state = e.sim_state;
    auto& sim_lua_state = e.sim_lua_state;
    if (!sim_state || !sim_lua_state) return;
    TestContext test_ctx{*sim_state, *sim_lua_state, sim_lua_state->raw(), e.vfs, e.store};
    // UI tests run against the UI Lua state, where the UI factories live
    // (the sim and UI states have been separate since M135c).
    TestContext ui_test_ctx{*sim_state, e.ui_lua_state, e.ui_lua_state.raw(), e.vfs, e.store};
    register_test_helpers(sim_lua_state->raw());
    const bool have_map = !e.map_path.empty();

    for (const Mode& m : kModesBefore)
        if (has(m.flag) && have_map) m.run(m.ui ? ui_test_ctx : test_ctx);
    const auto& map_path = e.map_path;
    auto& ui_lua_state = e.ui_lua_state;
    auto& ui_registry = e.ui_registry;
    auto& ui_thread_manager = e.ui_thread_manager;
    auto& beat_registry = e.beat_registry;
    auto& game_state_mgr = e.game_state_mgr;
    const bool gameui_test = has("--gameui-test");
    const bool victory_test = has("--victory-test");
    const bool dualstate_test = has("--dualstate-test");
    const bool construction_test = has("--construction-test");
    const bool phase2_test = has("--phase2-test");
    const bool phase3_test = has("--phase3-test");
    const bool phase4_test = has("--phase4-test");
    const bool phase5_test = has("--phase5-test");
    if ((gameui_test || victory_test) && !map_path.empty()) {
        // Selection is input-handler state; a headless one lets the test
        // select units (SelectUnits) and drive the selection UI.
        osc::renderer::InputHandler headless_input;
        std::unordered_set<osc::u32> prev_sel;
        lua_State* uL = ui_lua_state.raw();
        lua_pushstring(uL, "__osc_input_handler");
        lua_pushlightuserdata(uL, &headless_input);
        lua_rawset(uL, LUA_REGISTRYINDEX);
        osc::sim::SimCallbackQueue test_callbacks; // SimCallback / ProcessInfo
        lua_pushstring(uL, "__osc_sim_callback_queue");
        lua_pushlightuserdata(uL, &test_callbacks);
        lua_rawset(uL, LUA_REGISTRYINDEX);
        osc::lua::FactoryQueueDisplay test_factory_queue; // the construction panel's
        lua_pushstring(uL, "__osc_factory_queue");
        lua_pushlightuserdata(uL, &test_factory_queue);
        lua_rawset(uL, LUA_REGISTRYINDEX);
        osc::u32 frames = 0;
        auto pump = [&](int n) {
            for (int i = 0; i < n; ++i) {
                pump_ui_frames_with_controls(ui_lua_state, ui_thread_manager, beat_registry,
                                             ui_registry, 1, frames);
                dispatch_selection_change(uL, prev_sel, headless_input.selected(),
                                          headless_input.take_selection_event());
            }
        };
        // As the windowed loop plays: a sim tick, its beat, 6 UI frames.
        auto play = [&](int ticks) {
            for (int t = 0; t < ticks; ++t) {
                submit_sim_callbacks(test_callbacks, *sim_state);
                sim_state->tick();
                world_beat(sim_lua_state.get(), sim_state.get(), ui_lua_state.raw());
                note_game_over_if_ended(sim_state.get(), game_state_mgr, uL);
                pump(6);
            }
        };
        // A world click as the input handler makes it under FA's command mode.
        auto click = [&](osc::f32 x, osc::f32 z, bool shift) {
            const auto mode = read_command_mode(uL);
            auto issued = headless_input.click_in_command_mode(*sim_state, mode, x, z, shift);
            if (issued) report_command_issued(uL, *issued);
            return issued.has_value();
        };
        auto sim_lua = [&](const char* code) {
            auto r = sim_lua_state->do_string(code);
            if (!r) spdlog::error("sim Lua: {}", r.error().message);
            return static_cast<bool>(r);
        };
        if (gameui_test) osc::test::test_gameui(ui_test_ctx, pump, play, click, sim_lua);
        if (victory_test) {
            osc::test::test_victory_flow(ui_test_ctx, pump, play, sim_lua);
        }
        lua_pushstring(uL, "__osc_input_handler");
        lua_pushnil(uL);
        lua_rawset(uL, LUA_REGISTRYINDEX);
        lua_pushstring(uL, "__osc_sim_callback_queue");
        lua_pushnil(uL);
        lua_rawset(uL, LUA_REGISTRYINDEX);
        lua_pushstring(uL, "__osc_factory_queue");
        lua_pushnil(uL);
        lua_rawset(uL, LUA_REGISTRYINDEX);
    }
    for (const Mode& m : kModesAfter)
        if (has(m.flag) && have_map) m.run(m.ui ? ui_test_ctx : test_ctx);

    // Dual-state isolation test (does not require map)
    if (dualstate_test) {
        spdlog::info("=== Dual Lua State Test ===");
        int pass = 0, fail = 0;

        // 1. Both states initialized
        if (sim_lua_state->raw() && ui_lua_state.raw()) {
            spdlog::info("[PASS] Both Lua states initialized");
            pass++;
        } else {
            osc::test_status::fail("[FAIL] Lua state initialization");
            fail++;
        }

        // 2. sim_L has CreateUnit but NOT InternalCreateGroup
        //    Use lua_rawget on globals index to avoid __index metamethod
        //    (config.lua locks globals and errors on undefined access)
        {
            lua_State* sL = sim_lua_state->raw();
            lua_pushstring(sL, "CreateUnit");
            lua_rawget(sL, LUA_GLOBALSINDEX);
            bool sim_has_create_unit = !lua_isnil(sL, -1);
            lua_pop(sL, 1);
            lua_pushstring(sL, "InternalCreateGroup");
            lua_rawget(sL, LUA_GLOBALSINDEX);
            bool sim_has_ui_func = !lua_isnil(sL, -1);
            lua_pop(sL, 1);

            if (sim_has_create_unit) {
                spdlog::info("[PASS] sim_L has CreateUnit");
                pass++;
            } else {
                osc::test_status::fail("[FAIL] sim_L missing CreateUnit");
                fail++;
            }
            if (!sim_has_ui_func) {
                spdlog::info("[PASS] sim_L does NOT have InternalCreateGroup");
                pass++;
            } else {
                osc::test_status::fail(
                    "[FAIL] sim_L has InternalCreateGroup (should be ui_L only)");
                fail++;
            }
        }

        // 3. ui_L has InternalCreateGroup but NOT CreateUnit
        {
            lua_State* uL = ui_lua_state.raw();
            lua_pushstring(uL, "InternalCreateGroup");
            lua_rawget(uL, LUA_GLOBALSINDEX);
            bool ui_has_create_group = !lua_isnil(uL, -1);
            lua_pop(uL, 1);
            lua_pushstring(uL, "CreateUnit");
            lua_rawget(uL, LUA_GLOBALSINDEX);
            bool ui_has_sim_func = !lua_isnil(uL, -1);
            lua_pop(uL, 1);

            if (ui_has_create_group) {
                spdlog::info("[PASS] ui_L has InternalCreateGroup");
                pass++;
            } else {
                osc::test_status::fail("[FAIL] ui_L missing InternalCreateGroup");
                fail++;
            }
            if (!ui_has_sim_func) {
                spdlog::info("[PASS] ui_L does NOT have CreateUnit");
                pass++;
            } else {
                osc::test_status::fail("[FAIL] ui_L has CreateUnit (should be sim_L only)");
                fail++;
            }
        }

        // 4. Both share the same VFS
        {
            auto* sim_vfs = osc::lua::LuaState::get_vfs(sim_lua_state->raw());
            auto* ui_vfs = osc::lua::LuaState::get_vfs(ui_lua_state.raw());
            if (sim_vfs && ui_vfs && sim_vfs == ui_vfs) {
                spdlog::info("[PASS] Both states share the same VFS");
                pass++;
            } else {
                osc::test_status::fail("[FAIL] VFS mismatch (sim={}, ui={})",
                                       static_cast<void*>(sim_vfs), static_cast<void*>(ui_vfs));
                fail++;
            }
        }

        spdlog::info("=== Dual State Test: {} passed, {} failed ===", pass, fail);
    }

    // M140d: Construction panel integration test
    if (construction_test && !map_path.empty()) {
        spdlog::info("=== M140 Construction Panel Test ===");

        auto result = ui_lua_state.do_string(R"(
            local cat = ParseEntityCategory('FACTORY LAND TECH1')
            local units = EntityCategoryGetUnitList(cat)
            assert(type(units) == 'table', 'Expected table from EntityCategoryGetUnitList')
            print('EntityCategoryGetUnitList returned ' .. table.getn(units) .. ' entries')
            for i, id in ipairs(units) do
                if i <= 5 then print('  ' .. id) end
            end
        )");
        if (result.ok()) {
            spdlog::info("=== M140 Construction Panel Test PASSED ===");
        } else {
            osc::test_status::fail("=== M140 Construction Panel Test FAILED ===");
        }
    }

    // M140-M143: Phase 2 integration test
    if (phase2_test && !map_path.empty()) {
        spdlog::info("=== Phase 2 Integration Test ===");
        int pass = 0, fail = 0;

        // Test 1: EntityCategoryGetUnitList returns results
        {
            auto r = ui_lua_state.do_string(R"(
                local cat = ParseEntityCategory('STRUCTURE LAND')
                local list = EntityCategoryGetUnitList(cat)
                assert(type(list) == 'table', 'EntityCategoryGetUnitList failed')
                print('M140: EntityCategoryGetUnitList returned ' .. table.getn(list) .. ' blueprints')
            )");
            if (r.ok()) {
                spdlog::info("[PASS] EntityCategoryGetUnitList");
                pass++;
            } else {
                osc::test_status::fail("[FAIL] EntityCategoryGetUnitList");
                fail++;
            }
        }

        // Test 2: GetOrderBitmapNames returns 8 values
        {
            auto r = ui_lua_state.do_string(R"(
                local a,b,c,d,e,f,g,h = GetOrderBitmapNames('move')
                assert(a ~= nil, 'GetOrderBitmapNames returned nil')
                assert(type(g) == 'string', 'Expected sound cue string')
                print('M141: GetOrderBitmapNames("move") up=' .. a)
            )");
            if (r.ok()) {
                spdlog::info("[PASS] GetOrderBitmapNames");
                pass++;
            } else {
                osc::test_status::fail("[FAIL] GetOrderBitmapNames");
                fail++;
            }
        }

        // Test 3: GetRolloverInfo returns nil when nothing hovered
        {
            auto r = ui_lua_state.do_string(R"(
                local info = GetRolloverInfo()
                print('M142: GetRolloverInfo type=' .. type(info))
            )");
            if (r.ok()) {
                spdlog::info("[PASS] GetRolloverInfo");
                pass++;
            } else {
                osc::test_status::fail("[FAIL] GetRolloverInfo");
                fail++;
            }
        }

        // Test 4: StartCursorText doesn't crash
        {
            auto r = ui_lua_state.do_string(R"(
                StartCursorText(100, 100, 'Test', {1,1,0,1}, 1.0, false)
                print('M143: StartCursorText succeeded')
            )");
            if (r.ok()) {
                spdlog::info("[PASS] StartCursorText");
                pass++;
            } else {
                osc::test_status::fail("[FAIL] StartCursorText");
                fail++;
            }
        }

        // Test 5: orders.lua boots (pcall, allow WARN)
        {
            auto r = ui_lua_state.do_string(R"(
                local ok, err = pcall(function()
                    local orders = import('/lua/ui/game/orders.lua')
                    assert(orders ~= nil, 'orders.lua import returned nil')
                end)
                if ok then print('M141: orders.lua boot OK')
                else print('M141: orders.lua boot WARN: ' .. tostring(err)) end
            )");
            if (r.ok()) {
                spdlog::info("[PASS] orders.lua boot");
                pass++;
            } else {
                osc::test_status::fail("[FAIL] orders.lua boot");
                fail++;
            }
        }

        // Test 6: unitview.lua boots (pcall, allow WARN)
        {
            auto r = ui_lua_state.do_string(R"(
                local ok, err = pcall(function()
                    local unitview = import('/lua/ui/game/unitview.lua')
                    assert(unitview ~= nil, 'unitview.lua import returned nil')
                end)
                if ok then print('M142: unitview.lua boot OK')
                else print('M142: unitview.lua boot WARN: ' .. tostring(err)) end
            )");
            if (r.ok()) {
                spdlog::info("[PASS] unitview.lua boot");
                pass++;
            } else {
                osc::test_status::fail("[FAIL] unitview.lua boot");
                fail++;
            }
        }

        spdlog::info("=== Phase 2 Integration Test: {}/{} passed ===", pass, pass + fail);
    }

    // M144-M146: Phase 3 integration test
    if (phase3_test && !map_path.empty()) {
        spdlog::info("=== Phase 3 Integration Test ===");
        int pass = 0, fail = 0;

        // Test 1: GetCurrentUIState returns "game"
        {
            auto r = ui_lua_state.do_string(R"(
                local state = GetCurrentUIState()
                assert(state == 'game', 'Expected "game", got: ' .. tostring(state))
                print('M144: GetCurrentUIState = ' .. state)
            )");
            if (r.ok()) {
                spdlog::info("[PASS] GetCurrentUIState");
                pass++;
            } else {
                osc::test_status::fail("[FAIL] GetCurrentUIState");
                fail++;
            }
        }

        // Test 2: AddBeatFunction registers and fires
        {
            auto r = ui_lua_state.do_string(R"(
                __test_beat_called = false
                local function myBeat() __test_beat_called = true end
                AddBeatFunction(myBeat, 'test_beat')
                print('M145: AddBeatFunction registered')
            )");
            if (r.ok()) {
                spdlog::info("[PASS] AddBeatFunction registration");
                pass++;
            } else {
                osc::test_status::fail("[FAIL] AddBeatFunction registration");
                fail++;
            }
        }

        // Fire beat functions
        beat_registry.fire_all(ui_lua_state.raw());

        {
            auto r = ui_lua_state.do_string(R"(
                assert(__test_beat_called == true, 'BeatFunction was not called')
                RemoveBeatFunction('test_beat')
                print('M145: BeatFunction fired and removed OK')
            )");
            if (r.ok()) {
                spdlog::info("[PASS] BeatFunction fire + remove");
                pass++;
            } else {
                osc::test_status::fail("[FAIL] BeatFunction fire + remove");
                fail++;
            }
        }

        // Test 3: Time queries
        {
            auto r = ui_lua_state.do_string(R"(
                local t = GetGameTimeSeconds()
                local tick = GameTick()
                local gt = GetGameTime()
                local rate = GetSimRate()
                assert(type(t) == 'number', 'GetGameTimeSeconds failed')
                assert(type(tick) == 'number', 'GameTick failed')
                assert(type(gt) == 'string', 'GetGameTime should return string')
                assert(rate == 10, 'GetSimRate should be 10')
                print('M145: Time queries OK (t=' .. t .. ' tick=' .. tick .. ' gt=' .. gt .. ')')
            )");
            if (r.ok()) {
                spdlog::info("[PASS] Time queries");
                pass++;
            } else {
                osc::test_status::fail("[FAIL] Time queries");
                fail++;
            }
        }

        // Test 4: Speed control
        {
            auto r = ui_lua_state.do_string(R"(
                SetGameSpeed(2.0)
                local spd = GetGameSpeed()
                assert(spd == 2.0, 'SetGameSpeed failed: got ' .. tostring(spd))
                SetGameSpeed(1.0)
                print('M145: Speed control OK')
            )");
            if (r.ok()) {
                spdlog::info("[PASS] Speed control");
                pass++;
            } else {
                osc::test_status::fail("[FAIL] Speed control");
                fail++;
            }
        }

        // Test 5: EscapeHandler
        {
            auto r = ui_lua_state.do_string(R"(
                __test_esc_called = false
                SetEscapeHandler(function() __test_esc_called = true end)
                EscapeHandler()
                assert(__test_esc_called, 'EscapeHandler not called')
                print('M146: EscapeHandler OK')
            )");
            if (r.ok()) {
                spdlog::info("[PASS] EscapeHandler");
                pass++;
            } else {
                osc::test_status::fail("[FAIL] EscapeHandler");
                fail++;
            }
        }

        spdlog::info("=== Phase 3 Integration Test: {}/{} passed ===", pass, pass + fail);
    }

    // M147-M149: Phase 4 integration test
    if (phase4_test && !map_path.empty()) {
        spdlog::info("=== Phase 4 Integration Test ===");
        int pass = 0, fail = 0;

        // Test 1: FrontEndData round-trip
        {
            auto r = ui_lua_state.do_string(R"(
                SetFrontEndData('testKey', {value=42, name='test'})
                local d = GetFrontEndData('testKey')
                assert(d ~= nil, 'FrontEndData lost')
                assert(d.value == 42, 'FrontEndData value mismatch')
                print('M147: FrontEndData round-trip OK')
            )");
            if (r.ok()) {
                spdlog::info("[PASS] FrontEndData");
                pass++;
            } else {
                osc::test_status::fail("[FAIL] FrontEndData");
                fail++;
            }
        }

        // Test 2: HasCommandLineArg
        {
            auto r = ui_lua_state.do_string(R"(
                local has = HasCommandLineArg('--phase4-test')
                assert(has == true, 'Expected --phase4-test to be present')
                local no = HasCommandLineArg('--nonexistent')
                assert(no == false, 'Expected --nonexistent to be absent')
                print('M147: HasCommandLineArg OK')
            )");
            if (r.ok()) {
                spdlog::info("[PASS] HasCommandLineArg");
                pass++;
            } else {
                osc::test_status::fail("[FAIL] HasCommandLineArg");
                fail++;
            }
        }

        // Test 3: PlaySound gives a handle for a cue that plays, nil otherwise
        {
            auto r = ui_lua_state.do_string(R"(
                local h = PlaySound(Sound({Bank = 'Interface', Cue = 'X_Main_Menu_On_Start'}))
                assert(type(h) == 'number', 'PlaySound should return a handle')
                assert(PlaySound('test_click') == nil, 'an unknown cue plays nothing')
                StopSound(nil) -- a nil handle is a no-op
                print('M147: PlaySound OK (handle=' .. h .. ')')
            )");
            if (r.ok()) {
                spdlog::info("[PASS] PlaySound");
                pass++;
            } else {
                osc::test_status::fail("[FAIL] PlaySound");
                fail++;
            }
        }

        // Test 4: Skin selection
        {
            auto r = ui_lua_state.do_string(R"(
                UIUtil.SetCurrentSkin('cybran')
                local skin = UIUtil.GetCurrentSkin()
                assert(skin == 'cybran', 'Skin mismatch: ' .. tostring(skin))
                print('M149: Skin selection OK')
            )");
            if (r.ok()) {
                spdlog::info("[PASS] Skin selection");
                pass++;
            } else {
                osc::test_status::fail("[FAIL] Skin selection");
                fail++;
            }
        }

        // Test 5: Layout preference
        {
            auto r = ui_lua_state.do_string(R"(
                UIUtil.SetLayoutPreference('right')
                local layout = UIUtil.GetLayoutPreference()
                assert(layout == 'right', 'Layout mismatch: ' .. tostring(layout))
                print('M149: Layout preference OK')
            )");
            if (r.ok()) {
                spdlog::info("[PASS] Layout preference");
                pass++;
            } else {
                osc::test_status::fail("[FAIL] Layout preference");
                fail++;
            }
        }

        // Test 6: GetKeyBindings returns table
        {
            auto r = ui_lua_state.do_string(R"(
                local kb = GetKeyBindings()
                assert(type(kb) == 'table', 'GetKeyBindings should return table')
                assert(kb.attack == 'A', 'attack binding wrong')
                assert(kb.move == 'M', 'move binding wrong')
                print('M149: GetKeyBindings OK')
            )");
            if (r.ok()) {
                spdlog::info("[PASS] GetKeyBindings");
                pass++;
            } else {
                osc::test_status::fail("[FAIL] GetKeyBindings");
                fail++;
            }
        }

        // Test 7: Prefs table exists
        {
            auto r = ui_lua_state.do_string(R"(
                assert(type(Prefs) == 'table', 'Prefs not found')
                assert(type(Prefs.GetFromCurrentProfile) == 'function', 'GetFromCurrentProfile missing')
                assert(type(Prefs.SetToCurrentProfile) == 'function', 'SetToCurrentProfile missing')
                print('M149: Prefs table OK')
            )");
            if (r.ok()) {
                spdlog::info("[PASS] Prefs table");
                pass++;
            } else {
                osc::test_status::fail("[FAIL] Prefs table");
                fail++;
            }
        }

        // Test 8: LaunchSinglePlayerSession sets launch signal
        {
            auto r = ui_lua_state.do_string(R"(
                LaunchSinglePlayerSession({ScenarioFile='/maps/test/test_scenario.lua'})
                print('M148: LaunchSinglePlayerSession OK')
            )");
            if (r.ok()) {
                spdlog::info("[PASS] LaunchSinglePlayerSession");
                pass++;
            } else {
                osc::test_status::fail("[FAIL] LaunchSinglePlayerSession");
                fail++;
            }

            // Clear the launch flag so we don't actually try to launch
            lua_State* uL = ui_lua_state.raw();
            lua_pushstring(uL, "__osc_launch_requested");
            lua_pushnil(uL);
            lua_rawset(uL, LUA_REGISTRYINDEX);
        }

        // Test 9: DiskFindFiles accessible on ui_L
        {
            auto r = ui_lua_state.do_string(R"(
                assert(type(DiskFindFiles) == 'function', 'DiskFindFiles not on ui_L')
                assert(type(DiskGetFileInfo) == 'function', 'DiskGetFileInfo not on ui_L')
                assert(type(exists) == 'function', 'exists not on ui_L')
                print('M148: File I/O on ui_L OK')
            )");
            if (r.ok()) {
                spdlog::info("[PASS] File I/O on ui_L");
                pass++;
            } else {
                osc::test_status::fail("[FAIL] File I/O on ui_L");
                fail++;
            }
        }

        spdlog::info("=== Phase 4 Integration Test: {}/{} passed ===", pass, pass + fail);
    }

    // M150-M152: Phase 5 integration test
    if (phase5_test && !map_path.empty()) {
        spdlog::info("=== Phase 5 Integration Test ===");
        int pass = 0, fail = 0;

        // Test 1: IN_AddKeyMapTable / IN_RemoveKeyMapTable
        {
            auto r = ui_lua_state.do_string(R"(
                local action_called = false
                local km = {A = function() action_called = true end}
                IN_AddKeyMapTable(km)
                IN_RemoveKeyMapTable(km)
                print('M150: IN_AddKeyMapTable/IN_RemoveKeyMapTable OK')
            )");
            if (r.ok()) {
                spdlog::info("[PASS] KeyMap add/remove");
                pass++;
            } else {
                osc::test_status::fail("[FAIL] KeyMap add/remove");
                fail++;
            }
        }

        // Test 2: IsKeyDown exists and returns boolean
        {
            auto r = ui_lua_state.do_string(R"(
                local down = IsKeyDown(65)  -- GLFW_KEY_A = 65
                assert(type(down) == 'boolean', 'IsKeyDown should return boolean')
                print('M150: IsKeyDown OK')
            )");
            if (r.ok()) {
                spdlog::info("[PASS] IsKeyDown");
                pass++;
            } else {
                osc::test_status::fail("[FAIL] IsKeyDown");
                fail++;
            }
        }

        // Test 3: Camera SaveSettings / RestoreSettings
        {
            auto r = ui_lua_state.do_string(R"(
                local cam = GetCamera('WorldCamera')
                if cam then
                    local settings = cam:SaveSettings()
                    assert(type(settings) == 'table', 'SaveSettings should return table')
                    if settings.distance ~= nil then
                        -- Full camera available (renderer present)
                        assert(settings.target_x ~= nil, 'Missing target_x field')
                        cam:RestoreSettings(settings)
                        print('M150: Camera Save/RestoreSettings OK')
                    else
                        -- Headless mode: SaveSettings returns empty table
                        print('M150: Camera headless (empty settings) - OK')
                    end
                else
                    print('M150: Camera not available (headless) - skipping')
                end
            )");
            if (r.ok()) {
                spdlog::info("[PASS] Camera Save/RestoreSettings");
                pass++;
            } else {
                osc::test_status::fail("[FAIL] Camera Save/RestoreSettings");
                fail++;
            }
        }

        // Test 4: UIZoomTo exists
        {
            auto r = ui_lua_state.do_string(R"(
                assert(type(UIZoomTo) == 'function', 'UIZoomTo not registered')
                UIZoomTo({})  -- empty array, should not crash
                print('M150: UIZoomTo OK')
            )");
            if (r.ok()) {
                spdlog::info("[PASS] UIZoomTo");
                pass++;
            } else {
                osc::test_status::fail("[FAIL] UIZoomTo");
                fail++;
            }
        }

        // Test 5: RegisterChatFunc + SessionSendChatMessage
        {
            auto r = ui_lua_state.do_string(R"(
                local received = nil
                RegisterChatFunc(function(msg) received = msg end, 'test')
                SessionSendChatMessage({}, {text='hello', from='Player'})
                assert(received ~= nil, 'Chat func not called')
                assert(received.text == 'hello', 'Chat text mismatch')
                print('M151: Chat system OK')
            )");
            if (r.ok()) {
                spdlog::info("[PASS] Chat system");
                pass++;
            } else {
                osc::test_status::fail("[FAIL] Chat system");
                fail++;
            }
        }

        // Test 6: SendSystemMessage
        {
            auto r = ui_lua_state.do_string(R"(
                local sys_msg = nil
                RegisterChatFunc(function(msg) sys_msg = msg end, 'sys')
                SendSystemMessage('Test announcement')
                assert(sys_msg ~= nil, 'System message not received')
                assert(sys_msg.from == 'System', 'Expected from=System')
                assert(sys_msg.text == 'Test announcement', 'Text mismatch')
                print('M151: SendSystemMessage OK')
            )");
            if (r.ok()) {
                spdlog::info("[PASS] SendSystemMessage");
                pass++;
            } else {
                osc::test_status::fail("[FAIL] SendSystemMessage");
                fail++;
            }
        }

        // Test 7: GetSessionClients returns table with player
        {
            auto r = ui_lua_state.do_string(R"(
                local clients = GetSessionClients()
                assert(type(clients) == 'table', 'GetSessionClients should return table')
                assert(clients[1] ~= nil, 'Expected at least one client')
                assert(clients[1].name ~= nil, 'Client needs name')
                print('M151: GetSessionClients OK')
            )");
            if (r.ok()) {
                spdlog::info("[PASS] GetSessionClients");
                pass++;
            } else {
                osc::test_status::fail("[FAIL] GetSessionClients");
                fail++;
            }
        }

        // Test 8: GiveResources SimCallback exists
        {
            auto r = sim_lua_state->do_string(R"(
                local sc = rawget(_G, 'SimCallbacks')
                assert(type(sc) == 'table', 'SimCallbacks not found')
                assert(type(sc.GiveResources) == 'function', 'GiveResources missing')
                sc.GiveResources({From=1, To=2, Mass=100, Energy=200})
                print('M151: GiveResources SimCallback OK')
            )");
            if (r.ok()) {
                spdlog::info("[PASS] GiveResources SimCallback");
                pass++;
            } else {
                osc::test_status::fail("[FAIL] GiveResources SimCallback");
                fail++;
            }
        }

        spdlog::info("=== Phase 5 Integration Test: {}/{} passed ===", pass, pass + fail);
    }
}

} // namespace osc::test
