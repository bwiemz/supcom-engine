#include "integration_tests.hpp"
#include "intel_probe.hpp"
#include "render_probe.hpp"

#include "map/terrain.hpp"
#include "renderer/camera.hpp"
#include "renderer/mesh_cache.hpp"
#include "renderer/renderer.hpp"
#include "renderer/texture_cache.hpp"
#include "renderer/unit_renderer.hpp"
#include "sim/entity.hpp"
#include "sim/sim_state.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iterator>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace osc::test {

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::vector<std::string> placeholders(renderer::Renderer& r) {
    renderer::TextureCache& tex = r.texture_cache();
    std::vector<std::string> out;
    for (const renderer::MeshDrawGroup& g : r.unit_renderer().mesh_groups()) {
        if (!g.mesh || g.instance_count == 0) {
            continue;
        }
        const std::pair<const std::string*, bool> files[] = {
            {&g.mesh->texture_path, g.texture_ds == tex.fallback_descriptor()},
            {&g.mesh->specteam_path, g.specteam_ds == tex.specteam_fallback_descriptor()},
            {&g.mesh->normal_path, g.normal_ds == tex.normal_fallback_descriptor()},
            {&g.mesh->lookup_path, g.lookup_ds == tex.specteam_fallback_descriptor()},
            {&g.mesh->secondary_path, g.secondary_ds == tex.specteam_fallback_descriptor()},
        };
        for (const auto& [path, fallback] : files) {
            if (!path->empty() && fallback && (tex.pending(*path) || tex.get(*path))) {
                out.push_back(*path);
            }
        }
    }
    return out;
}

std::set<std::string> drawn_albedos(renderer::Renderer& r) {
    std::set<std::string> out;
    for (const renderer::MeshDrawGroup& g : r.unit_renderer().mesh_groups()) {
        if (g.mesh && g.instance_count > 0) {
            out.insert(lower(g.mesh->texture_path));
        }
    }
    return out;
}

bool draws(const std::set<std::string>& albedos, const std::string& folder) {
    return std::any_of(albedos.begin(), albedos.end(),
                       [&](const std::string& a) { return a.find(folder) != std::string::npos; });
}

std::string listed(const std::vector<std::string>& paths) {
    std::string s;
    for (size_t i = 0; i < paths.size() && i < 6; ++i) {
        s += (i ? ", " : "") + paths[i];
    }
    return paths.size() > 6 ? s + fmt::format(" and {} more", paths.size() - 6) : s;
}

} // namespace

void test_mesh_load(TestContext& ctx) {
    spdlog::info("=== MESH LOAD TEST: no mesh drawn without its textures ===");
    Tally t;

    struct Tree {
        f32 x, z;
    };
    std::vector<Tree> trees;
    ctx.sim.entity_registry().for_each([&](const sim::Entity& e) {
        if (e.is_prop() && !e.destroyed() &&
            lower(e.blueprint_id()).find("/props/trees/") != std::string::npos) {
            trees.push_back({e.position().x, e.position().z});
        }
    });
    Tree grove{0, 0};
    size_t most = 0;
    for (const Tree& a : trees) {
        size_t around = 0;
        for (const Tree& b : trees) {
            if (std::hypot(a.x - b.x, a.z - b.z) < 40.0f) {
                ++around;
            }
        }
        if (around > most) {
            most = around;
            grove = a;
        }
    }
    if (most < 10) {
        t.check(false, fmt::format("a grove ({} trees within 40)", most));
        return;
    }

    OffscreenShots shots(ctx);
    if (!shots.ok()) {
        t.check(false, "renderer init (no Vulkan?)");
        return;
    }
    renderer::Renderer& r = shots.renderer();

    (void)shots.shoot_frame(*ctx.sim.terrain(), grove.x, grove.z, 40.0f);
    const std::set<std::string> near = drawn_albedos(r);
    r.camera().set_eye_distance(220.0f);
    shots.redraw();
    const std::set<std::string> far = drawn_albedos(r);
    std::vector<std::string> fresh;
    std::set_difference(far.begin(), far.end(), near.begin(), near.end(),
                        std::back_inserter(fresh));
    const auto missing = placeholders(r);
    t.check(!fresh.empty() && missing.empty(),
            fmt::format("Test 1: zoomed out over {} trees, the first frame draws {} LOD textures "
                        "not drawn near ({}), and no placeholder for: {}",
                        most, fresh.size(), listed(fresh), listed(missing)));

    const auto spot = quiet_spot(ctx.sim);
    if (!spot) {
        t.check(false, "Test 2: a quiet spot");
        return;
    }
    const sim::EntityId loyalist = spawn_unit(ctx, "__osc_ml_loyalist", "url0303", "ARMY_1", *spot);
    ctx.sim.tick();
    shots.recapture();
    shots.redraw_until_loaded();
    r.camera().set_target(spot->x, spot->z);
    r.camera().set_eye_distance(40.0f);
    shots.redraw();
    const auto missing2 = placeholders(r);
    t.check(static_cast<bool>(loyalist) && draws(drawn_albedos(r), "/units/url0303/") &&
                missing2.empty(),
            fmt::format("Test 2: the Loyalist, made out of view, is drawn the first frame it is "
                        "in view, and no placeholder for: {}",
                        listed(missing2)));

    run_lua(ctx, "__osc_ml_loyalist:SetMesh('/units/xsl0303/xsl0303_mesh', true)");
    ctx.sim.tick();
    shots.recapture();
    shots.redraw();
    const std::set<std::string> swapping = drawn_albedos(r);
    const auto missing3 = placeholders(r);
    shots.redraw_until_loaded();
    const std::set<std::string> swapped = drawn_albedos(r);
    t.check(draws(swapping, "/units/url0303/") && !draws(swapping, "/units/xsl0303/") &&
                missing3.empty() && draws(swapped, "/units/xsl0303/") &&
                !draws(swapped, "/units/url0303/"),
            fmt::format("Test 3: the first frame after SetMesh draws its old mesh ({}), then "
                        "the new one ({}); no placeholder for: {}",
                        draws(swapping, "/units/url0303/"), draws(swapped, "/units/xsl0303/"),
                        listed(missing3)));

    (void)spawn_unit(ctx, "__osc_ml_titan", "uel0303", "ARMY_1", {spot->x + 10.0f, spot->z});
    ctx.sim.tick();
    shots.recapture();
    shots.redraw();
    const bool drawn_at_once = draws(drawn_albedos(r), "/units/uel0303/");
    const auto missing4 = placeholders(r);
    shots.redraw_until_loaded();
    t.check(!drawn_at_once && missing4.empty() && draws(drawn_albedos(r), "/units/uel0303/"),
            fmt::format("Test 4: the Titan, made in view, waits for its textures (drawn at "
                        "once: {}), then is drawn; no placeholder for: {}",
                        drawn_at_once, listed(missing4)));

    run_lua(ctx, R"(
        local lod = rawget(_G, '__blueprints')['/units/uel0106/uel0106_mesh'].LODs[1]
        lod.AlbedoName = '/osc_ml_missing_albedo.dds'
        lod.NormalsName = '/osc_ml_missing_normals.dds'
    )");
    (void)spawn_unit(ctx, "__osc_ml_bot", "uel0106", "ARMY_1", {spot->x - 10.0f, spot->z});
    ctx.sim.tick();
    shots.recapture();
    shots.redraw_until_loaded();
    const renderer::GPUTexture* fails =
        r.texture_cache().get("/textures/engine/b_fails_to_load.dds");
    const renderer::MeshDrawGroup* bot = nullptr;
    for (const renderer::MeshDrawGroup& g : r.unit_renderer().mesh_groups()) {
        if (g.mesh && g.instance_count > 0 &&
            g.mesh->texture_path == "/osc_ml_missing_albedo.dds") {
            bot = &g;
        }
    }
    t.check(fails && bot && bot->texture_ds == fails->descriptor_set &&
                bot->normal_ds == fails->descriptor_set,
            fmt::format("Test 5: a bot naming missing albedo and normals is drawn ({}) with "
                        "b_fails_to_load.dds in their place (loaded: {})",
                        bot != nullptr, fails != nullptr));

    spdlog::info("=== MESH LOAD TEST: {} passed, {} failed ===", t.pass, t.fail);
}

} // namespace osc::test
