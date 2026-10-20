// --gpu-pass-timing-test: the GPU's frame time split by pass (M223b), as the
// render bench asks for it, and a frame's two timestamps alone without it.

#include "app/support.hpp"
#include "core/test_status.hpp"
#include "integration_tests.hpp"
#include "map/heightmap.hpp"
#include "map/terrain.hpp"
#include "render_probe.hpp"
#include "renderer/gpu_queries.hpp"
#include "renderer/renderer.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <cstdlib>
#include <vector>

namespace osc::test {

namespace {

constexpr u32 kSize = 64;

} // namespace

void test_gpu_pass_timing(TestContext& ctx) {
    spdlog::info("=== GPU pass timing test ===");
    Tally t;
    OffscreenShots shots(ctx);
    if (!shots.ok()) {
        t.check(false, "renderer init (no Vulkan?)");
        return;
    }
    renderer::Renderer& r = shots.renderer();
    if (!r.gpu_timestamps_supported()) {
        // A skip must not drop a failure already recorded: report it and fail instead
        const int code = test_status::failure_count() > 0
                             ? app::finish_test_run("gpu-pass-timing-test")
                             : app::kExitSkippedNoData;
        if (code == app::kExitSkippedNoData)
            spdlog::warn("GPU pass timing test: the device counts no timestamps; skipped");
        spdlog::default_logger()->flush();
        std::exit(code);
    }
    // Ground under water, so the frame copies itself for the water to refract
    std::vector<u16> heights(static_cast<size_t>(kSize + 1) * (kSize + 1), 5 * 128);
    map::Terrain ground(map::Heightmap(kSize, kSize, 1.0f / 128.0f, std::move(heights)), 10.0f,
                        true);
    (void)shots.shoot(ground, kSize * 0.5f, kSize * 0.5f, 80.0f, false);

    using Frame = renderer::GpuFrameQueries::Frame;
    using Pass = renderer::GpuFrameQueries::Pass;
    const auto pass = [](const Frame& f, Pass p) { return f.pass_ms[static_cast<size_t>(p)]; };
    // A few frames drawn, so the last one's results are in (a frame in flight
    // behind), each a later frame than the call before's
    u64 last_sequence = 0;
    const auto frames = [&](const char* what) {
        for (int i = 0; i < 4; ++i) shots.redraw();
        const Frame f = r.last_gpu_frame();
        t.check(f.sequence > last_sequence && f.timed,
                fmt::format("{}: frame {} collected and timed (after frame {})", what, f.sequence,
                            last_sequence));
        last_sequence = f.sequence;
        return f;
    };

    const Frame whole = frames("Test 1");
    t.check(!whole.passes_timed && whole.gpu_ms > 0.0,
            fmt::format("Test 2: not timing passes, the frame is timed whole ({:.3f} ms) and not "
                        "split",
                        whole.gpu_ms));

    r.set_timing_passes(true);
    (void)frames("Test 3"); // the frames recorded before the switch drain out
    const Frame split = frames("Test 4");
    t.check(split.passes_timed && pass(split, Pass::Main) > 0.0 &&
                pass(split, Pass::Composite) + pass(split, Pass::Bloom) > 0.0,
            fmt::format("Test 5: timing passes, the frame ({:.3f} ms) splits into shadow {:.3f}, "
                        "normals {:.3f}, reflection {:.3f}, main {:.3f}, refraction copy {:.3f}, "
                        "bloom {:.3f}, composite {:.3f}, other {:.3f}",
                        split.gpu_ms, pass(split, Pass::Shadow), pass(split, Pass::Normals),
                        pass(split, Pass::Reflection), pass(split, Pass::Main),
                        pass(split, Pass::RefractionCopy), pass(split, Pass::Bloom),
                        pass(split, Pass::Composite), pass(split, Pass::Other)));

    // A pass's span can be zero long on a driver, so its mark is checked apart from its time
    for (const Pass p : {Pass::Shadow, Pass::Normals, Pass::Reflection, Pass::Main,
                         Pass::RefractionCopy, Pass::Bloom, Pass::Composite}) {
        const auto i = static_cast<size_t>(p);
        t.check(split.pass_marked[i], fmt::format("Test 5b: the {} pass was marked",
                                                  renderer::GpuFrameQueries::kPassNames[i]));
    }

    r.set_timing_passes(false);
    (void)frames("Test 6");
    const Frame after = frames("Test 7");
    t.check(!after.passes_timed, "Test 8: timing turned off again, the frame is timed whole");

    spdlog::info("GPU pass timing test: {} passed, {} failed", t.pass, t.fail);
}

} // namespace osc::test
