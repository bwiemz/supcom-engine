#include <catch2/catch_test_macros.hpp>

#include "renderer/renderer.hpp"
#include "renderer/texture_cache.hpp"
#include "renderer/vk_cmd.hpp"
#include "support/memory_mount.hpp"
#include "support/vulkan_device.hpp"
#include "vfs/virtual_file_system.hpp"

#include <array>
#include <cstring>
#include <memory>
#include <vector>

namespace {

std::vector<char> white_dds() {
    constexpr osc::u32 kSide = 4;
    std::vector<char> d(128 + kSide * kSide * 4, static_cast<char>(0xFF));
    std::memset(d.data(), 0, 128);
    const auto put = [&](size_t offset, osc::u32 v) { std::memcpy(d.data() + offset, &v, 4); };
    std::memcpy(d.data(), "DDS ", 4);
    put(4, 124);
    put(8, 0x1 | 0x2 | 0x4 | 0x8 | 0x1000);
    put(12, kSide);
    put(16, kSide);
    put(20, kSide * 4);
    put(28, 1);
    put(76, 32);
    put(80, 0x40 | 0x1);
    put(88, 32);
    put(92, 0x00FF0000);
    put(96, 0x0000FF00);
    put(100, 0x000000FF);
    put(104, 0xFF000000);
    put(108, 0x1000);
    return d;
}

} // namespace

TEST_CASE("Textures loaded between frames don't wait for the queue", "[renderer][texture_cache]") {
    if (!osc::test::vulkan_device()) SKIP("no Vulkan device");
    auto files = std::make_unique<osc::test::MemoryMount>();
    files->add("/textures/blocking.dds", white_dds());
    files->add("/textures/async.dds", white_dds());
    osc::vfs::VirtualFileSystem vfs;
    vfs.mount("/", std::move(files));
    osc::renderer::Renderer r;
    REQUIRE(r.init(64, 64, "uploads", /*offscreen=*/true));
    r.init_ui_caches(&vfs);
    (void)osc::renderer::take_command_counts();

    osc::renderer::TextureCache& textures = r.texture_cache();
    CHECK(textures.get_blocking("/textures/blocking.dds") != nullptr);
    CHECK(textures.get("/textures/async.dds") == nullptr);
    textures.wait_reads();
    textures.flush_uploads();
    CHECK(textures.get("/textures/async.dds") != nullptr);
    CHECK(textures.get_raw("raw", white_dds()) != nullptr);
    const std::array<osc::u8, 16> pixels{};
    CHECK(textures.upload_rgba("pixels", pixels.data(), 2, 2) != nullptr);
    CHECK(osc::renderer::take_command_counts().upload_waits == 0);
}
