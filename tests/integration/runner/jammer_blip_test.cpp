// --jammer-blip-test (M215e): a jammer's fake blips, as Moho's recon keeps
// them. On dry ground away from the starts: ARMY_2's field engineer
// (JammerBlips 5, JamRadius 12) and, 70 away, ARMY_1's radar (115 across
// its reach, 20 of sight). With its Jammer on and ARMY_1's radar on it,
// ARMY_1 holds five fakes within 12 of it, which its radar senses and it
// can't tell from real (the engineer's own blip beside them), and its
// influence map counts them as the engineer's threat (M215f); a sight of
// the spot unmasks them, and they count no more; the Jammer off, or the
// engineer gone, they go. The offsets come from the sim's stream, so a
// replay repeats them.

#include "integration_tests.hpp"
#include "intel_probe.hpp"
#include "render_probe.hpp"

#include "lua/lua_state.hpp"
#include "sim/influence_map.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/world_snapshot.hpp"

#include <spdlog/spdlog.h>

#include <cmath>
#include <string>
#include <vector>

namespace osc::test {

void test_jammer_blips(TestContext& ctx) {
    spdlog::info("=== JAMMER BLIP TEST: a jammer's fake blips (M215e) ===");
    Tally t;
    const auto spot = quiet_spot(ctx.sim);
    if (!spot) {
        t.check(false, "dry ground 60 from every unit");
        return;
    }
    const f32 sx = spot->x;
    const f32 sz = spot->z;
    const u32 engineer = spawn_unit(ctx, "__osc_jb_engineer", "xel0209", "ARMY_2", {sx, sz});
    (void)spawn_unit(ctx, "__osc_jb_radar", "ueb3101", "ARMY_1", {sx, sz + 70});
    const auto lua = [&](const std::string& code) { run_lua(ctx, code); };
    lua("__osc_jb_engineer:SetFireState(1)\n__osc_jb_engineer:SetImmobile(true)\n"
        "__osc_jb_radar:EnableIntel('Radar')\n");
    const auto key = sim::SimState::jam_key(engineer, 0);
    const auto offsets = [&]() -> const std::vector<sim::Vector3>* {
        const auto& all = ctx.sim.jam_offsets();
        const auto it = all.find(key);
        return it == all.end() ? nullptr : &it->second;
    };
    const auto shown = [&] {
        sim::WorldSnapshot snap;
        sim::capture_world(ctx.sim, snap);
        size_t n = 0;
        for (const auto& f : snap.fake_blips) n += f.source == engineer && f.viewer == 0 ? 1 : 0;
        return n;
    };
    const auto same = [](const std::vector<sim::Vector3>& a, const std::vector<sim::Vector3>& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (a[i].x != b[i].x || a[i].y != b[i].y || a[i].z != b[i].z) return false;
        return true;
    };
    const auto run = [&](int ticks) {
        for (int i = 0; i < ticks; ++i) ctx.sim.tick();
    };
    // ARMY_1's influence map: its entries for the engineer's fakes, and
    // ARMY_2's threat about the engineer (summed every 30 ticks)
    sim::InfluenceMap* threat = ctx.sim.influence_map(0);
    const auto fake_entries = [&] {
        size_t n = 0;
        if (threat)
            threat->for_each_entry([&](const sim::InfluenceMap::EntryView& v) {
                n += (v.id & sim::kFakeBlipBit) != 0 && v.source_army == 1 ? 1 : 0;
            });
        return n;
    };
    const auto threat_about = [&] {
        if (!threat) return 0.0f;
        const i32 c = threat->cell_of({sx, 0, sz});
        return threat->threat_rect(c % threat->width(), c / threat->width(), 1, nullptr,
                                   sim::ThreatType::Overall, 1);
    };

    lua("__osc_jb_engineer:DisableIntel('Jammer')\n");
    run(5);
    t.check(!offsets(), "a jammer with its Jammer off gives no fakes");

    lua("__osc_jb_engineer:EnableIntel('Jammer')\n");
    run(5);
    const auto* fakes = offsets();
    bool within = fakes != nullptr && fakes->size() == 5;
    if (fakes)
        for (const auto& o : *fakes) within = within && std::hypot(o.x, o.z) <= 12.0f && o.y == 0;
    t.check(within, "the army its radar reaches holds 5 fakes within JamRadius 12");
    t.check(!ctx.sim.jam_offsets().count(sim::SimState::jam_key(engineer, 1)),
            "none for the jammer's own army");
    const size_t sensed = shown();
    t.check(sensed == 5,
            "all five are sensed by radar and not known fake (" + std::to_string(sensed) + ")");
    const std::vector<sim::Vector3> before = fakes ? *fakes : std::vector<sim::Vector3>{};
    run(3);
    t.check(offsets() && same(*offsets(), before), "a fake keeps its place about the jammer");

    // Fed as the engineer (SurfaceThreatLevel 1) at their own places, and
    // summed at the map's next update
    run(31);
    const size_t held = fake_entries();
    t.check(held == 5, "ARMY_1's influence map holds the 5 fakes (" + std::to_string(held) + ")");
    const f32 fooled = threat_about();

    // Sight over the engineer: the fakes it covers are known fake, and add
    // no threat from the next update on.
    lua("CreateVisibleAreaAtPoint(1, " + std::to_string(sx) + ", 0, " + std::to_string(sz) +
        ", 30, 5)\n");
    run(2);
    t.check(shown() == 0, "seen, the fakes are known fake");
    run(31);
    const f32 seen = threat_about();
    t.check(std::abs(fooled - seen - 5.0f) < 1e-3f, "known fake, they add no threat (" +
                                                        std::to_string(fooled) + " -> " +
                                                        std::to_string(seen) + ")");

    // The sight gone (its 5 seconds out), radar still holding the engineer:
    // known fake once, known fake for good (Moho's UpdateBlip keeps
    // RECON_KnownFake), so they show and add no threat as before.
    run(30);
    const sim::Entity* jammer = ctx.sim.entity_registry().find(engineer);
    t.check(jammer &&
                (ctx.sim.recon_of(*jammer, 0) &
                 (sim::SimState::kReconLOS | sim::SimState::kReconOmni)) == 0 &&
                (ctx.sim.recon_of(*jammer, 0) & sim::SimState::kReconRadar) != 0,
            "the sight gone, the radar still on the engineer");
    t.check(offsets() != nullptr && shown() == 0,
            "out of sight again, the fakes stay known fake (" + std::to_string(shown()) +
                " shown)");
    run(31);
    const f32 after = threat_about();
    t.check(std::abs(after - seen) < 1e-3f, "and still add no threat (" + std::to_string(seen) +
                                                " -> " + std::to_string(after) + ")");

    lua("__osc_jb_engineer:DisableIntel('Jammer')\n");
    run(3);
    t.check(!offsets() && shown() == 0, "the Jammer off, its fakes go");
    lua("__osc_jb_engineer:EnableIntel('Jammer')\n");
    run(3);
    t.check(offsets() && !same(*offsets(), before), "on again, they are drawn afresh");
    lua("__osc_jb_engineer:Destroy()\n");
    run(3);
    t.check(!offsets(), "the jammer gone, its fakes go");

    spdlog::info("=== JAMMER BLIP TEST: {} passed, {} failed ===", t.pass, t.fail);
}

} // namespace osc::test
