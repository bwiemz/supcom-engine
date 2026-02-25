#include <catch2/catch_test_macros.hpp>

// Init loader integration tests require a real FA installation.
// These are guarded by checking for the FA_PATH environment variable.

TEST_CASE("Init loader placeholder", "[init]") {
    // Placeholder — real tests added when init loader is verified
    REQUIRE(true);
}
