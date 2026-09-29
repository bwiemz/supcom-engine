// Logs and bug reports (M228b): the log's file gets what was logged before
// it was chosen, earlier runs' logs are kept, and --collect-logs's zip holds
// what a bug report needs.

#include <catch2/catch_test_macros.hpp>

#include "app/log_bundle.hpp"
#include "core/log.hpp"

#include <minizip/unzip.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <random>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path;
    TempDir() {
        std::random_device rd;
        path = fs::temp_directory_path() /
               ("osc_logs_test_" + std::to_string(rd()) + std::to_string(rd()));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&&) = delete;
    TempDir& operator=(TempDir&&) = delete;
};

void write_file(const fs::path& p, const std::string& contents) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << contents;
}

/// A file's text, its line ends as "\n" (spdlog writes "\r\n" on Windows).
std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    std::string s = text.str();
    std::erase(s, '\r');
    return s;
}

/// A logger writing its messages alone to `sink`.
std::shared_ptr<spdlog::logger> logger_to(const std::shared_ptr<osc::log::DeferredFileSink>& sink) {
    auto logger = std::make_shared<spdlog::logger>("logs_test", sink);
    logger->set_pattern("%v");
    return logger;
}

/// A zip's entries: name -> contents.
std::map<std::string, std::string> read_zip(const fs::path& zip) {
    std::map<std::string, std::string> entries;
    unzFile file = unzOpen64(zip.string().c_str());
    REQUIRE(file != nullptr);
    for (int at = unzGoToFirstFile(file); at == UNZ_OK; at = unzGoToNextFile(file)) {
        char name[256];
        unz_file_info64 info{};
        REQUIRE(unzGetCurrentFileInfo64(file, &info, name, sizeof(name), nullptr, 0, nullptr, 0) ==
                UNZ_OK);
        REQUIRE(unzOpenCurrentFile(file) == UNZ_OK);
        std::string contents(static_cast<size_t>(info.uncompressed_size), '\0');
        if (!contents.empty())
            REQUIRE(
                unzReadCurrentFile(file, contents.data(), static_cast<unsigned>(contents.size())) ==
                static_cast<int>(contents.size()));
        unzCloseCurrentFile(file);
        entries[name] = contents;
    }
    unzClose(file);
    return entries;
}

} // namespace

TEST_CASE("The log's file gets what was logged before it was chosen", "[logs]") {
    TempDir tmp;
    auto sink = std::make_shared<osc::log::DeferredFileSink>();
    auto logger = logger_to(sink);
    logger->info("before");
    logger->info("still before");
    CHECK_FALSE(sink->is_open());

    const fs::path file = tmp.path / "logs" / "opensupcom.log";
    REQUIRE(sink->open(file));
    logger->info("after");
    logger->flush();
    CHECK(read_file(file) == "before\nstill before\nafter\n");

    // Opened once: another file changes nothing
    REQUIRE(sink->open(tmp.path / "other.log"));
    logger->info("later");
    logger->flush();
    CHECK_FALSE(fs::exists(tmp.path / "other.log"));
    CHECK(read_file(file).ends_with("after\nlater\n"));
}

TEST_CASE("A log file that can't be opened keeps the lines held", "[logs]") {
    TempDir tmp;
    write_file(tmp.path / "a_file", "");
    auto sink = std::make_shared<osc::log::DeferredFileSink>();
    auto logger = logger_to(sink);
    logger->info("held");
    CHECK_FALSE(sink->open(tmp.path / "a_file" / "opensupcom.log")); // under a file
    CHECK_FALSE(sink->is_open());
    REQUIRE(sink->open(tmp.path / "opensupcom.log"));
    logger->flush();
    CHECK(read_file(tmp.path / "opensupcom.log") == "held\n");
}

TEST_CASE("Earlier runs' logs are kept, the oldest dropped", "[logs]") {
    TempDir tmp;
    const fs::path log = tmp.path / "opensupcom.log";
    write_file(log, "run 3");
    write_file(tmp.path / "opensupcom.1.log", "run 2");
    write_file(tmp.path / "opensupcom.2.log", "run 1");

    osc::log::rotate(log, 2);
    CHECK_FALSE(fs::exists(log));
    CHECK(read_file(tmp.path / "opensupcom.1.log") == "run 3");
    CHECK(read_file(tmp.path / "opensupcom.2.log") == "run 2");
    CHECK_FALSE(fs::exists(tmp.path / "opensupcom.3.log"));

    // No log yet: nothing moves
    osc::log::rotate(log, 2);
    CHECK(read_file(tmp.path / "opensupcom.1.log") == "run 3");
    CHECK(read_file(tmp.path / "opensupcom.2.log") == "run 2");
}

TEST_CASE("The --collect-logs zip holds the logs, crash reports and settings", "[logs]") {
    TempDir tmp;
    osc::app::LogBundleSources sources;
    sources.logs_dir = tmp.path / "state" / "logs";
    sources.crash_dir = tmp.path / "state" / "crashes";
    sources.settings_file = tmp.path / "config" / "settings.json";
    sources.system = "OpenSupCom test\n";
    write_file(sources.logs_dir / "opensupcom.log", "[info] Vulkan GPU: Test GPU\n");
    write_file(sources.logs_dir / "opensupcom.1.log", "earlier\n");
    write_file(sources.logs_dir / "notes.txt", "not a log\n");
    write_file(sources.crash_dir / "crash-1790000000-42.txt", "*** OpenSupCom crashed\n");
    write_file(sources.crash_dir / "core", "not a report\n");
    write_file(sources.settings_file, R"({"fa_path": "/games/fa"})");

    const fs::path zip = tmp.path / "bundle.zip";
    write_file(zip, "an old bundle");
    const auto files = osc::app::write_log_bundle(zip, sources);
    REQUIRE(files == 5);
    const auto entries = read_zip(zip);
    CHECK(entries.size() == 5);
    CHECK(entries.at("system.txt") == "OpenSupCom test\n");
    CHECK(entries.at("logs/opensupcom.log") == "[info] Vulkan GPU: Test GPU\n");
    CHECK(entries.at("logs/opensupcom.1.log") == "earlier\n");
    CHECK(entries.at("crashes/crash-1790000000-42.txt") == "*** OpenSupCom crashed\n");
    CHECK(entries.at("settings.json") == R"({"fa_path": "/games/fa"})");

    // The system report names the build and the GPU the latest log names
    const std::string report = osc::app::system_report(sources.logs_dir);
    CHECK(report.find("OpenSupCom ") == 0);
    CHECK(report.find("GPU: Test GPU") != std::string::npos);
    // ...or, when the latest run crashed before choosing one, the run before
    write_file(sources.logs_dir / "opensupcom.log", "crashed early\n");
    write_file(sources.logs_dir / "opensupcom.1.log", "[info] Vulkan GPU: Earlier GPU\n");
    write_file(sources.logs_dir / "opensupcom.2.log", "[info] Vulkan GPU: Oldest GPU\n");
    CHECK(osc::app::system_report(sources.logs_dir).find("GPU: Earlier GPU") != std::string::npos);
    CHECK(osc::app::system_report(tmp.path / "none").find("GPU: unknown") != std::string::npos);
}

TEST_CASE("A bundle with no logs yet still says what the system is", "[logs]") {
    TempDir tmp;
    osc::app::LogBundleSources sources;
    sources.logs_dir = tmp.path / "missing";
    sources.crash_dir = tmp.path / "missing";
    sources.settings_file = tmp.path / "missing.json";
    sources.system = "system\n";
    const auto files = osc::app::write_log_bundle(tmp.path / "bundle.zip", sources);
    REQUIRE(files == 1);
    CHECK(read_zip(tmp.path / "bundle.zip").count("system.txt") == 1);
    // An unwritable place fails cleanly
    CHECK_FALSE(osc::app::write_log_bundle(tmp.path / "missing" / "x" / "b.zip", sources));
}
