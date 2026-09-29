// The engine's secrets (M208c-c): key bytes from the OS's random source,
// and a file only its owner reads.

#include <catch2/catch_test_macros.hpp>

#include "platform/secrets.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

namespace fs = std::filesystem;

TEST_CASE("Secure random bytes differ each time, and fill what is asked", "[platform][secrets]") {
    std::array<unsigned char, 600> a{}, b{}; // more than getentropy's 256 at once
    REQUIRE(osc::platform::secure_random(a.data(), a.size()));
    REQUIRE(osc::platform::secure_random(b.data(), b.size()));
    CHECK(a != b);
    int zeros = 0;
    for (unsigned char c : a) zeros += c == 0;
    CHECK(zeros < 20); // about 2 expected in 600 bytes
}

TEST_CASE("A private file is written whole, and readable by its owner alone",
          "[platform][secrets]") {
    std::random_device rd;
    const fs::path dir = fs::temp_directory_path() / ("osc-secrets-" + std::to_string(rd()));
    fs::create_directories(dir);
    const fs::path file = dir / "key";
    const std::string first = "first secret", second = "second";
    REQUIRE(osc::platform::write_private_file(file, first.data(), first.size()));
    REQUIRE(
        osc::platform::write_private_file(file, second.data(), second.size())); // replaced whole
    std::ifstream in(file, std::ios::binary);
    const std::string got((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(got == second);
    CHECK_FALSE(fs::exists(dir / "key.part"));
#ifndef _WIN32
    const fs::perms p = fs::status(file).permissions();
    CHECK((p & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none);
#endif
    in.close();
    fs::remove_all(dir);
}
