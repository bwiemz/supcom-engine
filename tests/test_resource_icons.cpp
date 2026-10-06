#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "renderer/camera.hpp"
#include "renderer/minimap_renderer.hpp"
#include "renderer/resource_icon_renderer.hpp"
#include "ui/world_view.hpp"

#include <cmath>
#include <vector>

using namespace osc;
using namespace osc::renderer;
using Catch::Approx;
using sim::ResourceDeposit;

namespace {

constexpr f32 kW = 800.0f;
constexpr f32 kH = 600.0f;
constexpr f32 kHalfPi = 1.5707964f;
const PlayableRect kWholeMap{0, 0, 512, 512};

ResourceDeposit deposit(f32 x, f32 z, f32 size, ResourceDeposit::Type type) {
    ResourceDeposit d;
    d.x = x;
    d.z = z;
    d.size = size;
    d.type = type;
    return d;
}

Camera camera_at(f32 x, f32 z, f32 zoom) {
    Camera cam;
    cam.init(512.0f, 512.0f);
    cam.set_input_enabled(false);
    cam.set_viewport(kW, kH);
    cam.set_zoom(zoom);
    cam.set_pitch(0.87f);
    cam.set_target(x, z);
    return cam;
}

std::vector<ResourceIcon> icons_for(const std::vector<ResourceDeposit>& deposits, const Camera& cam,
                                    const PlayableRect& rect = kWholeMap) {
    return resource_icons(deposits, cam, kW, kH, rect, [](f32, f32) { return 0.0f; });
}

} // namespace

TEST_CASE("A deposit's icon sits at its footprint's centre", "[renderer][resources]") {
    const auto mass = deposit_centre(deposit(10.5f, 20.5f, 1.0f, ResourceDeposit::Mass));
    CHECK(mass[0] == 10.5f);
    CHECK(mass[1] == 20.5f);
    const auto hydro = deposit_centre(deposit(100.5f, 50.5f, 3.0f, ResourceDeposit::Hydrocarbon));
    CHECK(hydro[0] == 100.5f);
    CHECK(hydro[1] == 50.5f);
    const auto snapped = deposit_centre(deposit(10.3f, 20.9f, 2.0f, ResourceDeposit::Mass));
    CHECK(snapped[0] == 10.0f);
    CHECK(snapped[1] == 20.0f);
}

TEST_CASE("A deposit poking out of the playable rect has no icon", "[renderer][resources]") {
    const PlayableRect rect{100, 100, 200, 200};
    CHECK(deposit_in_rect(deposit(100.5f, 150.5f, 1.0f, ResourceDeposit::Mass), rect));
    CHECK(deposit_in_rect(deposit(199.5f, 150.5f, 1.0f, ResourceDeposit::Mass), rect));
    CHECK_FALSE(deposit_in_rect(deposit(99.5f, 150.5f, 1.0f, ResourceDeposit::Mass), rect));
    CHECK_FALSE(deposit_in_rect(deposit(199.5f, 150.5f, 3.0f, ResourceDeposit::Hydrocarbon), rect));
    CHECK_FALSE(deposit_in_rect(deposit(150.5f, 200.5f, 1.0f, ResourceDeposit::Mass), rect));

    const std::vector<ResourceDeposit> deposits = {
        deposit(150.5f, 150.5f, 1.0f, ResourceDeposit::Mass),
        deposit(99.5f, 150.5f, 1.0f, ResourceDeposit::Mass)};
    CHECK(icons_for(deposits, camera_at(150.0f, 150.0f, 200.0f), rect).size() == 1);
}

TEST_CASE("Deposits show icons once the view is wider than UI_ResourceLODCutoff",
          "[renderer][resources]") {
    const std::vector<ResourceDeposit> deposits = {
        deposit(256.5f, 256.5f, 1.0f, ResourceDeposit::Mass)};
    CHECK(icons_for(deposits, camera_at(256.5f, 256.5f, kResourceLodCutoff * 0.98f)).empty());
    const auto shown = icons_for(deposits, camera_at(256.5f, 256.5f, kResourceLodCutoff * 1.02f));
    REQUIRE(shown.size() == 1);
    CHECK(shown[0].type == ResourceDeposit::Mass);
}

TEST_CASE("A deposit's icon is at its whole pixel, and only on the screen",
          "[renderer][resources]") {
    const Camera cam = camera_at(256.0f, 256.0f, 300.0f);
    const std::vector<ResourceDeposit> deposits = {
        deposit(256.5f, 256.5f, 1.0f, ResourceDeposit::Mass),
        deposit(270.5f, 240.5f, 3.0f, ResourceDeposit::Hydrocarbon),
        deposit(500.5f, 256.5f, 1.0f, ResourceDeposit::Mass)};
    const auto icons = icons_for(deposits, cam);
    REQUIRE(icons.size() == 2);
    const auto vp = cam.view_proj(kW / kH);
    for (size_t i = 0; i < icons.size(); ++i) {
        const ResourceDeposit& d = deposits[i];
        const f32 cx = vp[0] * d.x + vp[8] * d.z + vp[12];
        const f32 cy = vp[1] * d.x + vp[9] * d.z + vp[13];
        const f32 cw = vp[3] * d.x + vp[11] * d.z + vp[15];
        CHECK(icons[i].x == std::floor((cx / cw + 1.0f) * 0.5f * kW));
        CHECK(icons[i].y == std::floor((cy / cw + 1.0f) * 0.5f * kH));
        CHECK(icons[i].type == d.type);
    }
}

TEST_CASE("A resource icon's glow pulses as primbatcher.fx's ResourceIconPS",
          "[renderer][resources]") {
    CHECK(resource_icon_glow(1.0f, kHalfPi, 0.0f, 0.0f) == Approx(0.6f));
    CHECK(resource_icon_glow(0.5f, kHalfPi, 0.0f, 0.0f) == Approx(0.3f));
    CHECK(resource_icon_glow(1.0f, 0.0f, 0.0f, 0.0f) == Approx(0.0f).margin(1e-6));
    const f32 ten_r = 10.0f * 0.00277f * 500.0f;
    CHECK(resource_icon_glow(1.0f, ten_r, 300.0f, 400.0f) == Approx(0.0f).margin(1e-5));
    CHECK(resource_icon_glow(1.0f, ten_r + kHalfPi, 300.0f, 400.0f) == Approx(0.6f));
    CHECK(resource_icon_glow(1.0f, ten_r + 0.5f, 300.0f, 400.0f) ==
          Approx(0.6f * std::sin(0.5f) * std::sin(0.5f)));
}

TEST_CASE("A minimap shows the deposits inside the playable rect", "[renderer][resources]") {
    const std::vector<ResourceDeposit> deposits = {
        deposit(256.5f, 128.5f, 1.0f, ResourceDeposit::Mass),
        deposit(500.5f, 500.5f, 3.0f, ResourceDeposit::Hydrocarbon)};
    const MapArea area{10.0f, 20.0f, 200.0f, 200.0f};
    const auto all = minimap_resource_icons(deposits, area, 512.0f, 512.0f, kWholeMap);
    REQUIRE(all.size() == 2);
    CHECK(all[0].x == 110.0f);
    CHECK(all[0].y == 70.0f);
    CHECK(all[1].type == ResourceDeposit::Hydrocarbon);
    const auto inside =
        minimap_resource_icons(deposits, area, 512.0f, 512.0f, PlayableRect{0, 0, 400, 400});
    REQUIRE(inside.size() == 1);
    CHECK(inside[0].type == ResourceDeposit::Mass);
}

TEST_CASE("A world view renders resources until told not to", "[ui][worldview][resources]") {
    ui::WorldView view;
    CHECK(view.resource_rendering());
    view.set_resource_rendering(false);
    CHECK_FALSE(view.resource_rendering());
}
