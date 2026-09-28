// --map-parse-test: every map's .scmap is read to its last byte.
//
// 26 of retail's 60 maps had been read short: the 17 dry ones (the parser
// skipped their water block, which the file holds whatever the water flag
// says) and the 9 with over 10,000 wave generators (a "sanity" limit). Each
// loaded without its strata, decals, normal maps, terrain types and props.

#include "core/test_status.hpp"
#include "integration_tests.hpp"
#include "map/scmap_parser.hpp"
#include "render_probe.hpp"
#include "vfs/virtual_file_system.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <optional>
#include <string>
#include <vector>

namespace osc::test {

namespace {

std::optional<map::ScmapData> parse(vfs::VirtualFileSystem& vfs, const std::string& path) {
    const auto bytes = vfs.read_file(path);
    if (!bytes) return std::nullopt;
    auto result = map::parse_scmap(std::vector<u8>(bytes->begin(), bytes->end()));
    if (!result.ok()) return std::nullopt;
    return std::move(result.value());
}

} // namespace

void test_map_parse(TestContext& ctx) {
    spdlog::info("=== Map parse test ===");
    Tally t;

    // Test 1: every map is read whole
    {
        const std::vector<std::string> files = ctx.vfs.find_files("/maps", "*.scmap");
        std::vector<std::string> short_read;
        for (const std::string& f : files) {
            const auto d = parse(ctx.vfs, f);
            if (!d || !d->read_whole) short_read.push_back(f);
        }
        std::string which;
        for (const std::string& f : short_read) which += " " + f;
        t.check(files.size() >= 60 && short_read.empty(),
                fmt::format("Test 1: {} maps, {} read to the last byte (short:{})", files.size(),
                            files.size() - short_read.size(), which.empty() ? " none" : which));
    }

    // Test 2: a dry map (Drake's Ravine) past its water block: its decals and
    // props
    {
        const auto d = parse(ctx.vfs, "/maps/SCMP_003/SCMP_003.scmap");
        t.check(d && !d->has_water && d->water_elevation == 0.0f && d->decals.size() > 1000 &&
                    d->props.size() > 1000 && d->strata.size() == 10,
                fmt::format("Test 2: SCMP_003, water off, has {} decals and {} props",
                            d ? d->decals.size() : 0, d ? d->props.size() : 0));
    }

    // Test 3: a map with 34,468 wave generators, each read, and what follows
    {
        const auto d = parse(ctx.vfs, "/maps/SCMP_030/SCMP_030.scmap");
        bool sane = d && !d->waves.empty();
        if (d) {
            for (const map::ScmapWaveGenerator& g : d->waves) {
                sane = sane && !g.texture.empty() && !g.ramp.empty() && g.frame_count >= 1.0f &&
                       g.strip_count >= 1.0f && g.lifetime[0] <= g.lifetime[1] &&
                       g.interval[0] <= g.interval[1] && g.position[0] >= 0.0f &&
                       g.position[0] <= static_cast<f32>(d->map_width) && g.position[2] >= 0.0f &&
                       g.position[2] <= static_cast<f32>(d->map_height);
            }
        }
        t.check(d && d->waves.size() == 34468 && sane && d->props.size() > 1000,
                fmt::format("Test 3: SCMP_030 has {} wave generators ({}) and {} props after them",
                            d ? d->waves.size() : 0,
                            sane ? "each on the map, in range" : "not sane",
                            d ? d->props.size() : 0));
    }

    // Test 4: bytes past the props aren't a whole read (a misaligned parse
    // can end on the props' count yet short of the end)
    {
        auto bytes = ctx.vfs.read_file("/maps/SCMP_009/SCMP_009.scmap");
        bool whole = false;
        bool padded_whole = true;
        if (bytes) {
            std::vector<u8> data(bytes->begin(), bytes->end());
            auto plain = map::parse_scmap(data);
            whole = plain.ok() && plain.value().read_whole;
            data.insert(data.end(), 4, 0);
            auto padded = map::parse_scmap(data);
            padded_whole = padded.ok() && padded.value().read_whole;
        }
        t.check(whole && !padded_whole,
                fmt::format("Test 4: SCMP_009 is read whole ({}), with 4 bytes more it isn't ({})",
                            whole, !padded_whole));
    }

    spdlog::info("Map parse test: {} passed, {} failed", t.pass, t.fail);
}

} // namespace osc::test
