#pragma once

struct lua_State;

namespace osc::sim { class SimState; }
namespace osc::lua { class LuaState; }
namespace osc::vfs { class VirtualFileSystem; }
namespace osc::blueprints { class BlueprintStore; }

namespace osc::test {

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
void test_terrain_tex(TestContext& ctx);
void test_shadow(TestContext& ctx);
void test_massstub4(TestContext& ctx);
void test_spatial(TestContext& ctx);
void test_unitsound(TestContext& ctx);
void test_medstub(TestContext& ctx);
void test_lowstub(TestContext& ctx);
void test_blend(TestContext& ctx);
void test_ui(TestContext& ctx);

} // namespace osc::test
