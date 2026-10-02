// --shield-render-test (M211k): FA's shields, drawn as FA draws them.
//
// Each faction's T2 shield generator, its shield up. The shield entity wears
// its blueprint's Mesh at SetDrawScale(Size), centred on the generator's
// middle plus the shield's vertical offset, with its depth fill (MeshZ)
// drawn just before it. It shows over the ground, its far side too once the
// fill is gone. A hit takes its health, which its instance carries, and
// adds an impact patch turned to face the shot, gone five seconds on.
// (Another army's shield out of the player's sight: --effect-intel-test's
// Test 7.)

#include "integration_tests.hpp"
#include "intel_probe.hpp"
#include "render_probe.hpp"

#include "core/image.hpp"
#include "map/terrain.hpp"
#include "renderer/mesh_cache.hpp"
#include "renderer/renderer.hpp"
#include "renderer/unit_renderer.hpp"
#include "sim/entity_registry.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <string>

namespace osc::test {

namespace {

using renderer::MeshTechnique;

/// Where the first group drawing `technique` is in this frame's draw order.
std::optional<size_t> order_of(const renderer::Renderer& r, MeshTechnique technique) {
    const auto& groups = r.unit_renderer().mesh_groups();
    for (size_t i = 0; i < groups.size(); ++i)
        if (groups[i].mesh && groups[i].mesh->technique == technique &&
            groups[i].instance_count > 0)
            return i;
    return std::nullopt;
}

/// How many instances this frame draws with `technique`.
u32 instances_of(const renderer::Renderer& r, MeshTechnique technique) {
    u32 n = 0;
    for (const auto& g : r.unit_renderer().mesh_groups())
        if (g.mesh && g.mesh->technique == technique) n += g.instance_count;
    return n;
}

/// The first instance this frame draws with `technique`, or null.
const renderer::MeshInstance* instance_of(const renderer::Renderer& r, MeshTechnique technique) {
    const renderer::MeshInstance* all = r.unit_renderer().mesh_instances();
    for (const auto& g : r.unit_renderer().mesh_groups())
        if (all && g.mesh && g.mesh->technique == technique && g.instance_count > 0)
            return &all[g.instance_offset];
    return nullptr;
}

/// The length of a model matrix's column: its scale along that axis.
f32 column_length(const f32* m, int column) {
    const f32* c = m + static_cast<size_t>(column) * 4;
    return std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
}

/// How blue the frame's middle is: the mean of blue less red.
f32 blueness(const Pixels& p) {
    f32 sum = 0.0f;
    for (const auto& px : p) sum += px[2] - px[0];
    return p.empty() ? 0.0f : sum / static_cast<f32>(p.size());
}

/// The share of the frame's middle darker in `a` than in `b` (by more than
/// 0.02 in its brightest channel's mean).
f32 darker_share(const Pixels& a, const Pixels& b) {
    size_t darker = 0;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
        const f32 la = (a[i][0] + a[i][1] + a[i][2]) / 3.0f;
        const f32 lb = (b[i][0] + b[i][1] + b[i][2]) / 3.0f;
        if (la < lb - 0.02f) ++darker;
    }
    return a.empty() ? 0.0f : static_cast<f32>(darker) / static_cast<f32>(a.size());
}

/// The mean difference of two scenes' alpha (the glow) over their middle
/// (35%-65% each way).
f32 mean_alpha_diff(const renderer::Renderer::SceneImage& a,
                    const renderer::Renderer::SceneImage& b) {
    if (a.width != b.width || a.height != b.height || a.width == 0) return 0.0f;
    f32 sum = 0.0f;
    u32 n = 0;
    for (u32 y = a.height * 35 / 100; y < a.height * 65 / 100; ++y)
        for (u32 x = a.width * 35 / 100; x < a.width * 65 / 100; ++x) {
            const size_t i = (static_cast<size_t>(y) * a.width + x) * 4 + 3;
            sum += std::abs(a.rgba[i] - b.rgba[i]);
            ++n;
        }
    return n ? sum / static_cast<f32>(n) : 0.0f;
}

/// The mean of one channel over the frame's middle.
f32 mean_channel(const Pixels& p, int channel) {
    f32 sum = 0.0f;
    for (const auto& px : p) sum += px[static_cast<size_t>(channel)];
    return p.empty() ? 0.0f : sum / static_cast<f32>(p.size());
}

bool near(f32 a, f32 b, f32 tolerance) {
    return std::abs(a - b) <= tolerance;
}

} // namespace

void test_shield_render(TestContext& ctx) {
    spdlog::info("=== Shield render test (M211k) ===");
    Tally t;
    const map::Terrain* terrain = ctx.sim.terrain();
    const auto spot = terrain ? quiet_spot(ctx.sim) : std::nullopt;
    if (!spot) {
        t.check(false, "the map, and dry ground 60 from every unit");
        return;
    }
    const Spot uef_at{spot->x, spot->z};
    const Spot cybran_at{spot->x + 40, spot->z};
    const Spot aeon_at{spot->x, spot->z + 50};
    const Spot seraphim_at{spot->x + 40, spot->z + 50};
    const u32 uef = spawn_unit(ctx, "__osc_sh_uef", "ueb4202", "ARMY_1", uef_at);
    (void)spawn_unit(ctx, "__osc_sh_cybran", "urb4202", "ARMY_1", cybran_at);
    (void)spawn_unit(ctx, "__osc_sh_aeon", "uab4202", "ARMY_1", aeon_at);
    (void)spawn_unit(ctx, "__osc_sh_seraphim", "xsb4202", "ARMY_1", seraphim_at);
    // Power to spare: a shield short of energy goes down (shield.lua's
    // OnState). Their shields come up once they're finished.
    run_lua(ctx, "ForkThread(function()\n"
                 "  while true do ArmyBrains[1]:GiveResource('ENERGY', 5000) WaitTicks(1) end\n"
                 "end)\n");
    for (int i = 0; i < 20; ++i) ctx.sim.tick();

    OffscreenShots shots(ctx);
    if (!shots.ok()) {
        t.check(false, "renderer init (no Vulkan?)");
        return;
    }
    renderer::Renderer& r = shots.renderer();
    const auto tick = [&](int ticks) {
        for (int i = 0; i < ticks; ++i) ctx.sim.tick();
        shots.recapture();
    };

    // OSC_SHIELD_TEST_PNG=<prefix>: the frames looked at, to see
    const char* dump = std::getenv("OSC_SHIELD_TEST_PNG");
    const auto keep = [&](const char* name, const ImageRGBA8& image) {
        if (dump) write_png(fmt::format("{}_{}.png", dump, name), image);
    };

    auto& registry = ctx.sim.entity_registry();
    const auto* gen_entity = registry.find(uef);
    const auto* gen =
        gen_entity && gen_entity->is_unit() ? static_cast<const sim::Unit*>(gen_entity) : nullptr;
    const sim::Entity* shield = gen ? registry.find(gen->shield_entity_id()) : nullptr;
    if (!gen || !shield) {
        t.check(false, "the UEF generator and its shield");
        return;
    }
    // Its middle (SizeY 2.5, half up), then the shield's offset (-3)
    const sim::Vector3 g = gen->position();
    const f32 centre_y = g.y + 1.25f - 3.0f;
    const auto centred = [&](const sim::Entity& e) {
        return near(e.position().x, g.x, 1e-3f) && near(e.position().y, centre_y, 1e-3f) &&
               near(e.position().z, g.z, 1e-3f);
    };
    // Its depth fill: a script entity on the generator wearing MeshZ
    const auto fill_of = [&]() -> const sim::Entity* {
        const sim::Entity* found = nullptr;
        registry.for_each([&](const sim::Entity& e) {
            if (!e.destroyed() && !e.is_shield() && e.parent_entity_id() == uef &&
                e.mesh_override().find("Shield01z") != std::string::npos)
                found = &e;
        });
        return found;
    };

    // Test 1: the shield and its fill sit at the generator's middle plus the
    // offset; drawn at the shield's size, the fill first
    {
        const sim::Entity* fill = fill_of();
        (void)shots.shoot(*terrain, uef_at.x, uef_at.z, 60.0f);
        const auto fill_order = order_of(r, MeshTechnique::ShieldFill);
        const auto shield_order = order_of(r, MeshTechnique::ShieldUEF);
        const renderer::MeshInstance* inst = instance_of(r, MeshTechnique::ShieldUEF);
        const f32 diameter = inst ? column_length(inst->model, 0) : 0.0f;
        const f32 drawn_y = inst ? inst->model[13] : 0.0f;
        t.check(
            fill && centred(*shield) && centred(*fill) && fill_order && shield_order &&
                *fill_order < *shield_order && inst && near(diameter, 26.0f, 1e-3f) &&
                near(drawn_y, centre_y, 1e-3f),
            fmt::format("Test 1: the shield at y {:.2f} and its fill at {:.2f} (want {:.2f}); "
                        "drawn {:.2f} across at {:.2f}, the fill {} in the order, the shield {}",
                        shield->position().y, fill ? fill->position().y : 0.0f, centre_y, diameter,
                        drawn_y, fill_order ? static_cast<int>(*fill_order) : -1,
                        shield_order ? static_cast<int>(*shield_order) : -1));
    }

    // Test 2: it shows, bluer than the ground without it; with its fill
    // gone, its far side shows through too
    {
        const ImageRGBA8 frame = shots.grab();
        keep("uef", frame);
        const Pixels on = centre_pixels(frame);
        run_lua(ctx, "__osc_sh_uef.MyShield:RemoveShield()\n");
        tick(1);
        const Pixels off = centre_pixels(shots.grab());
        run_lua(ctx, "__osc_sh_uef.MyShield:CreateShieldMesh()\n");
        tick(1);
        const Pixels filled = centre_pixels(shots.grab());
        run_lua(ctx, "local s = __osc_sh_uef.MyShield\n"
                     "s.MeshZ:Destroy() s.MeshZ = nil\n");
        tick(1);
        const Pixels unfilled = centre_pixels(shots.grab());
        run_lua(ctx, "__osc_sh_uef.MyShield:CreateShieldMesh()\n"); // the fill again
        tick(1);
        const f32 shown = mean_abs_diff(on, off);
        const f32 bluer = blueness(on) - blueness(off);
        const f32 front = mean_abs_diff(filled, off);
        const f32 both = mean_abs_diff(unfilled, off);
        t.check(shown > 0.01f && bluer > 0.01f && both > front * 1.1f,
                fmt::format("Test 2: the shield changes the frame by {:.4f}, bluer by {:.4f}; "
                            "{:.4f} with its fill, {:.4f} without",
                            shown, bluer, front, both));
    }

    // Test 3: its colour follows its health: weak, ShieldPS adds a red-heavy
    // glow and more alpha to it. That glow pulses with its clock, so the
    // strongest of a pulse's worth of ticks, each seen at full and at a
    // tenth.
    {
        f32 redder = -1.0f;
        f32 carried = -1.0f;
        for (int i = 0; i < 9; ++i) {
            const Pixels full = centre_pixels(shots.grab());
            run_lua(ctx,
                    "local s = __osc_sh_uef.MyShield s:SetHealth(s, s:GetMaxHealth() * 0.1)\n");
            shots.recapture();
            const Pixels weak = centre_pixels(shots.grab());
            if (const renderer::MeshInstance* inst = instance_of(r, MeshTechnique::ShieldUEF))
                carried = inst->parameter;
            run_lua(ctx, "local s = __osc_sh_uef.MyShield s:SetHealth(s, s:GetMaxHealth())\n");
            tick(2);
            redder = std::max(redder, mean_channel(weak, 0) - mean_channel(full, 0));
        }
        t.check(near(carried, 0.1f, 1e-3f) && redder > 0.005f,
                fmt::format("Test 3: at a tenth of its health (its instance's {:.3f}) the "
                            "shield is redder, by up to {:.4f}",
                            carried, redder));
    }

    // Test 3: a hit halves its health, which its instance carries, and adds
    // an impact facing the shot (drawn first: SortOrder 500), gone five
    // seconds on
    {
        run_lua(ctx, "__osc_sh_uef.MyShield:OnDamage(nil, 4500, Vector(1, 0, 0), 'Normal')\n");
        tick(2);
        keep("impact", shots.grab());
        const f32 health = shield->max_health() > 0 ? shield->health() / shield->max_health() : 0;
        const renderer::MeshInstance* inst = instance_of(r, MeshTechnique::ShieldUEF);
        const renderer::MeshInstance* impact = instance_of(r, MeshTechnique::ShieldImpact);
        const auto impact_order = order_of(r, MeshTechnique::ShieldImpact);
        const auto fill_order = order_of(r, MeshTechnique::ShieldFill);
        // OrientFromDir(-shot): its +z along (-1, 0, 0), at the shield's size
        const bool facing = impact && near(impact->model[8], -26.0f, 0.01f) &&
                            near(impact->model[9], 0.0f, 0.01f) &&
                            near(impact->model[10], 0.0f, 0.01f);
        const bool at_shield = impact && near(impact->model[12], g.x, 0.01f) &&
                               near(impact->model[13], centre_y, 0.01f) &&
                               near(impact->model[14], g.z, 0.01f);
        tick(50);
        shots.redraw();
        const u32 later = instances_of(r, MeshTechnique::ShieldImpact);
        t.check(near(health, 0.5f, 0.01f) && inst && near(inst->parameter, health, 1e-3f) &&
                    facing && at_shield && impact_order && fill_order &&
                    *impact_order < *fill_order && later == 0,
                fmt::format("Test 3: hit, its health {:.3f}, its instance's {:.3f}; the impact "
                            "{} and {}, {} in the order (the fill {}); {} impacts five seconds on",
                            health, inst ? inst->parameter : -1.0f,
                            facing ? "faces the shot" : "doesn't face the shot",
                            at_shield ? "sits on the shield" : "is elsewhere",
                            impact_order ? static_cast<int>(*impact_order) : -1,
                            fill_order ? static_cast<int>(*fill_order) : -1, later));
    }

    // Test 4: each faction's shield draws with its technique, and shows
    {
        struct Faction {
            const char* name;
            const char* global;
            Spot at;
            MeshTechnique technique;
        };
        const Faction factions[] = {
            {"Cybran", "__osc_sh_cybran", cybran_at, MeshTechnique::ShieldCybran},
            {"Aeon", "__osc_sh_aeon", aeon_at, MeshTechnique::ShieldAeon},
            {"Seraphim", "__osc_sh_seraphim", seraphim_at, MeshTechnique::ShieldSeraphim},
        };
        for (const Faction& f : factions) {
            const ImageRGBA8 frame = shots.shoot_frame(*terrain, f.at.x, f.at.z, 60.0f);
            keep(f.name, frame);
            const Pixels on = centre_pixels(frame);
            const u32 drawn = instances_of(r, f.technique);
            // Cybran's draws in two passes, the second pushed out
            const u32 passes = r.mesh_draws(f.technique);
            const u32 want_passes = f.technique == MeshTechnique::ShieldCybran ? 2 : 1;
            run_lua(ctx, fmt::format("{}.MyShield:RemoveShield()\n", f.global));
            tick(1);
            const Pixels off = centre_pixels(shots.grab());
            run_lua(ctx, fmt::format("{}.MyShield:CreateShieldMesh()\n", f.global));
            tick(1);
            const f32 shown = mean_abs_diff(on, off);
            // Seraphim's adds its colour (SrcAlpha, One): nothing behind it
            // darkens, but for what moved in the tick between (the trees)
            const f32 darker = darker_share(on, off);
            const bool added = f.technique != MeshTechnique::ShieldSeraphim || darker < 0.1f;
            t.check(drawn == 1 && passes == want_passes && shown > 0.005f && added,
                    fmt::format("Test 4: the {} shield draws {} time(s) in {} pass(es), "
                                "changing the frame by {:.4f}; {:.1f}% of it darker",
                                f.name, drawn, passes, shown, darker * 100.0f));
        }
        // All four, each with its fill
        (void)shots.shoot(*terrain, spot->x + 20, spot->z + 25, 150.0f);
        const u32 fills = instances_of(r, MeshTechnique::ShieldFill);
        const u32 shields = instances_of(r, MeshTechnique::ShieldUEF) +
                            instances_of(r, MeshTechnique::ShieldCybran) +
                            instances_of(r, MeshTechnique::ShieldAeon) +
                            instances_of(r, MeshTechnique::ShieldSeraphim);
        t.check(fills == 4 && shields == 4,
                fmt::format("Test 4: all four in view, {} shields and {} fills", shields, fills));
    }

    // Test 5: a personal shield (M211l). The Obsidian's is built in: up, its
    // owner wears the PhaseShield mesh, drawn as the unit, then its shell;
    // down, its own mesh again.
    {
        const Spot obsidian_at{spot->x + 20, spot->z + 25};
        (void)spawn_unit(ctx, "__osc_sh_obsidian", "ual0202", "ARMY_1", obsidian_at);
        tick(20);
        const ImageRGBA8 frame = shots.shoot_frame(*terrain, obsidian_at.x, obsidian_at.z, 6.0f);
        keep("obsidian", frame);
        const Pixels on = centre_pixels(frame);
        const u32 worn = instances_of(r, MeshTechnique::PhaseShield);
        // Its shell draws as PhaseShield; the unit under it as Unit
        const u32 shells = r.mesh_draws(MeshTechnique::PhaseShield);
        run_lua(ctx, "__osc_sh_obsidian.MyShield:RemoveShield()\n");
        tick(1);
        const ImageRGBA8 plain = shots.grab();
        const Pixels off = centre_pixels(plain);
        const u32 worn_after = instances_of(r, MeshTechnique::PhaseShield);
        const int itself = meshes_near(drawn(r), "ual0202_albedo", obsidian_at.x, 4.0f);
        const f32 shown = mean_abs_diff(on, off);
        t.check(worn == 1 && shells == 1 && worn_after == 0 && itself == 1 && shown > 0.005f,
                fmt::format("Test 5: the Obsidian's shield up, {} PhaseShield mesh, {} shell "
                            "drawn; down, {} (its own mesh {} time(s)); the shell changes the "
                            "frame by {:.4f}",
                            worn, shells, worn_after, itself, shown));

        // The UEF ACU wears its PhaseShield mesh as its warp-in does. Its
        // unit pass shades by another normal map than its own mesh's, but
        // writes the same glow (the specular's blue): the scene's alpha
        // changes by the shell alone.
        const Spot acu_at{spot->x + 10, spot->z + 40};
        (void)spawn_unit(ctx, "__osc_sh_acu", "uel0001", "ARMY_1", acu_at);
        tick(2);
        (void)shots.shoot(*terrain, acu_at.x, acu_at.z, 6.0f);
        const auto glow = [&]() {
            renderer::Renderer::SceneImage scene;
            r.request_scene_capture(
                [&](renderer::Renderer::SceneImage image) { scene = std::move(image); });
            shots.redraw();
            return scene;
        };
        const renderer::Renderer::SceneImage acu_bare = glow();
        run_lua(ctx, "__osc_sh_acu:SetMesh('/units/uel0001/UEL0001_PhaseShield_mesh', true)\n");
        tick(1);
        keep("acu", shots.grab());
        const renderer::Renderer::SceneImage acu_shell = glow();
        const f32 acu_glow = mean_alpha_diff(acu_shell, acu_bare);
        t.check(acu_glow > 0.005f,
                fmt::format("Test 5: the UEF ACU in its PhaseShield mesh, the shell changes the "
                            "frame's glow by {:.4f}",
                            acu_glow));

        // The Seraphim SCU's comes with its Shield enhancement: its
        // SeraphimPersonalShield mesh, the unit as Seraphim, then its shell
        // from the secondary texture
        const Spot scu_at{spot->x + 20, spot->z + 40};
        (void)spawn_unit(ctx, "__osc_sh_scu", "xsl0301", "ARMY_1", scu_at);
        tick(2);
        const ImageRGBA8 bare = shots.shoot_frame(*terrain, scu_at.x, scu_at.z, 6.0f);
        const u32 bare_seraphim = r.mesh_draws(MeshTechnique::Seraphim);
        run_lua(ctx, "__osc_sh_scu:CreateEnhancement('Shield')\n");
        tick(20);
        const ImageRGBA8 shielded = shots.grab();
        keep("scu", shielded);
        const u32 scu_worn = instances_of(r, MeshTechnique::SeraphimPersonalShield);
        // The unit under the shell draws as Seraphim, as it did bare
        const u32 scu_shells = r.mesh_draws(MeshTechnique::SeraphimPersonalShield);
        const u32 shielded_seraphim = r.mesh_draws(MeshTechnique::Seraphim);
        const f32 scu_shown = mean_abs_diff(centre_pixels(shielded), centre_pixels(bare));
        t.check(scu_worn == 1 && scu_shells == 1 && bare_seraphim >= 1 &&
                    shielded_seraphim == bare_seraphim && scu_shown > 0.005f,
                fmt::format("Test 5: the Seraphim SCU's shield, {} SeraphimPersonalShield mesh, "
                            "{} shell; {} Seraphim draws shielded, {} bare; the frame changes by "
                            "{:.4f}",
                            scu_worn, scu_shells, shielded_seraphim, bare_seraphim, scu_shown));
    }

    // Test 6: at Low (M211n) each faction's shield draws its LowFidelity
    // technique, in one pass, Cybran's too, and shows. Their colours, from
    // ThreeUVTexShiftScaleLoFiVS's reads: UEF's ShieldLoFiPS at least 0.45
    // bluer than red, so the frame bluer; Cybran's ShieldCybranLoFiPS (0.2,
    // 0, 0.5) by the specular, no green, where High's has its (0.15, 0.15,
    // 0.3); Seraphim's, Aeon's ShieldAeonLoFiPS, still added.
    {
        renderer::Renderer::VideoOptions& video = r.video_options();
        const renderer::Renderer::VideoOptions as_was = video;
        // The share of the frame's middle greener in `a` than in `b`
        const auto greener_share = [](const Pixels& a, const Pixels& b) {
            size_t greener = 0;
            for (size_t i = 0; i < a.size() && i < b.size(); ++i)
                if (a[i][1] > b[i][1] + 0.02f) ++greener;
            return a.empty() ? 0.0f : static_cast<f32>(greener) / static_cast<f32>(a.size());
        };
        struct Faction {
            const char* name;
            const char* global;
            Spot at;
            MeshTechnique technique;
        };
        const Faction factions[] = {
            {"UEF", "__osc_sh_uef", uef_at, MeshTechnique::ShieldUEF},
            {"Cybran", "__osc_sh_cybran", cybran_at, MeshTechnique::ShieldCybran},
            {"Aeon", "__osc_sh_aeon", aeon_at, MeshTechnique::ShieldAeon},
            {"Seraphim", "__osc_sh_seraphim", seraphim_at, MeshTechnique::ShieldSeraphim},
        };
        // A shield's look: its passes, then the frame's middle with it and
        // without
        struct Look {
            u32 passes = 0;
            f32 shown = 0.0f;
            f32 bluer = 0.0f;
            f32 greener = 0.0f;
            f32 darker = 0.0f;
        };
        const auto look = [&](const Faction& f) {
            const ImageRGBA8 frame = shots.shoot_frame(*terrain, f.at.x, f.at.z, 60.0f);
            keep(fmt::format("{}_{}", f.name, video.graphics_fidelity).c_str(), frame);
            const Pixels on = centre_pixels(frame);
            Look l;
            l.passes = r.mesh_draws(f.technique);
            run_lua(ctx, fmt::format("{}.MyShield:RemoveShield()\n", f.global));
            tick(1);
            const Pixels off = centre_pixels(shots.grab());
            run_lua(ctx, fmt::format("{}.MyShield:CreateShieldMesh()\n", f.global));
            tick(1);
            l.shown = mean_abs_diff(on, off);
            l.bluer = blueness(on) - blueness(off);
            l.greener = greener_share(on, off);
            l.darker = darker_share(on, off);
            return l;
        };
        const Look cybran_high = look(factions[1]);
        video.graphics_fidelity = 0;
        Look low[std::size(factions)];
        bool ok = cybran_high.greener > 0.02f;
        std::string seen;
        for (size_t i = 0; i < std::size(factions); ++i) {
            low[i] = look(factions[i]);
            ok = ok && low[i].passes == 1 && low[i].shown > 0.005f;
            seen += fmt::format(" {} {} pass(es), changing it by {:.4f};", factions[i].name,
                                low[i].passes, low[i].shown);
        }
        video = as_was;
        ok = ok && low[0].bluer > 0.01f && low[1].greener < 0.01f && low[3].darker < 0.1f;
        t.check(ok, fmt::format("Test 6: at Low:{} UEF's bluer by {:.4f}; Cybran's greener on "
                                "{:.1f}% ({:.1f}% at High); Seraphim's darker on {:.1f}%",
                                seen, low[0].bluer, low[1].greener * 100.0f,
                                cybran_high.greener * 100.0f, low[3].darker * 100.0f));
    }

    spdlog::info("Shield render test: {} passed, {} failed", t.pass, t.fail);
}

} // namespace osc::test
