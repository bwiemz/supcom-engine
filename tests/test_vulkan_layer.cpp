#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

TEST_CASE("The validation layer's manifest names a library the loader opens by its path",
          "[renderer]") {
#ifndef OSC_VULKAN_LAYER_PATH
    SKIP("built without the vulkan-validation feature");
#else
    const fs::path dir = OSC_VULKAN_LAYER_PATH;
    std::ifstream in(dir / "VkLayer_khronos_validation.json");
    REQUIRE(in);
    const fs::path library =
        nlohmann::json::parse(in).at("layer").at("library_path").get<std::string>();
    INFO("library_path: " << library.string());
    REQUIRE(library.has_parent_path());
    CHECK(fs::exists(library.is_absolute() ? library : dir / library));
#endif
}
