#include "app/log_bundle.hpp"

#include "core/version.hpp"
#include "platform/engine_settings.hpp"
#include "platform/system_info.hpp"

#include <minizip/zip.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <string_view>
#include <vector>

namespace osc::app {

namespace fs = std::filesystem;

namespace {

/// The regular files in `dir` whose names start with `prefix` and end with
/// `suffix`, by name.
std::vector<fs::path> files_in(const fs::path& dir, std::string_view prefix,
                               std::string_view suffix) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (it->is_regular_file(ec) && name.size() >= prefix.size() + suffix.size() &&
            name.compare(0, prefix.size(), prefix) == 0 &&
            name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
            out.push_back(it->path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

/// A zip being written; closed when it goes.
class ZipWriter {
public:
    explicit ZipWriter(const fs::path& path) : zip_(zipOpen64(path.string().c_str(), 0)) {}
    ~ZipWriter() {
        if (zip_) zipClose(zip_, nullptr);
    }
    ZipWriter(const ZipWriter&) = delete;
    ZipWriter& operator=(const ZipWriter&) = delete;
    ZipWriter(ZipWriter&&) = delete;
    ZipWriter& operator=(ZipWriter&&) = delete;

    [[nodiscard]] bool ok() const { return zip_ != nullptr; }

    /// Add `name`, its contents streamed from `read(buffer, size)` (the
    /// bytes read; 0 at the end).
    template <typename Read> bool add(const std::string& name, Read read) {
        zip_fileinfo info{};
        const std::time_t now = std::time(nullptr);
        if (const std::tm* t = std::localtime(&now)) {
            info.tmz_date = {t->tm_sec,  t->tm_min, t->tm_hour,
                             t->tm_mday, t->tm_mon, t->tm_year + 1900};
        }
        if (zipOpenNewFileInZip64(zip_, name.c_str(), &info, nullptr, 0, nullptr, 0, nullptr,
                                  Z_DEFLATED, Z_DEFAULT_COMPRESSION, 1) != ZIP_OK)
            return false;
        std::array<char, 64 * 1024> buffer{};
        bool ok = true;
        for (size_t n; ok && (n = read(buffer.data(), buffer.size())) > 0;)
            ok = zipWriteInFileInZip(zip_, buffer.data(), static_cast<unsigned>(n)) == ZIP_OK;
        return zipCloseFileInZip(zip_) == ZIP_OK && ok;
    }

    bool add_file(const std::string& name, const fs::path& file) {
        std::ifstream in(file, std::ios::binary);
        if (!in) return false;
        return add(name, [&](char* buffer, size_t size) {
            in.read(buffer, static_cast<std::streamsize>(size));
            return static_cast<size_t>(in.gcount());
        });
    }

    bool add_text(const std::string& name, const std::string& text) {
        size_t at = 0;
        return add(name, [&](char* buffer, size_t size) {
            const size_t n = std::min(size, text.size() - at);
            std::copy_n(text.data() + at, n, buffer);
            at += n;
            return n;
        });
    }

private:
    zipFile zip_;
};

/// A log's place in the rotation: opensupcom.log 0 (this run or the last),
/// opensupcom.<n>.log n (n runs before it).
int rotation_index(const fs::path& log) {
    const std::string stem = log.stem().string(); // opensupcom or opensupcom.<n>
    const auto dot = stem.find('.');
    if (dot == std::string::npos) return 0;
    try {
        return std::stoi(stem.substr(dot + 1));
    } catch (const std::exception&) {
        return 1 << 20;
    }
}

/// The GPU the most recent log naming one names ("Vulkan GPU: ..."): a run
/// that crashed before choosing one leaves the run before it to say.
std::string gpu_from_logs(const fs::path& logs_dir) {
    auto logs = files_in(logs_dir, "opensupcom", ".log");
    std::sort(logs.begin(), logs.end(), [](const fs::path& a, const fs::path& b) {
        return rotation_index(a) < rotation_index(b);
    });
    constexpr std::string_view key = "Vulkan GPU: ";
    for (const auto& log : logs) {
        std::ifstream in(log);
        for (std::string line; std::getline(in, line);) {
            if (const auto at = line.find(key); at != std::string::npos)
                return line.substr(at + key.size());
        }
    }
    return "unknown (no log names one)";
}

} // namespace

std::optional<int> write_log_bundle(const fs::path& zip, const LogBundleSources& sources) {
    std::error_code ec;
    fs::remove(zip, ec);
    ZipWriter out(zip);
    if (!out.ok()) return std::nullopt;
    int files = 0;
    if (!out.add_text("system.txt", sources.system)) return std::nullopt;
    ++files;
    for (const auto& log : files_in(sources.logs_dir, "opensupcom", ".log"))
        if (out.add_file("logs/" + log.filename().string(), log)) ++files;
    for (const auto& report : files_in(sources.crash_dir, "crash-", ".txt"))
        if (out.add_file("crashes/" + report.filename().string(), report)) ++files;
    if (fs::is_regular_file(sources.settings_file, ec) &&
        out.add_file("settings.json", sources.settings_file))
        ++files;
    return files;
}

std::string system_report(const fs::path& logs_dir) {
    return std::string(core::version_line()) + "\n" + "Build: " + core::build_id() + "\n" +
           "OS: " + platform::os_description() + "\n" + "GPU: " + gpu_from_logs(logs_dir) + "\n";
}

int collect_logs(const std::string& out) {
    const auto env = platform::system_env();
    LogBundleSources sources;
    sources.logs_dir = platform::engine_log_file(env).parent_path();
    sources.crash_dir = platform::engine_crash_dir(env);
    sources.settings_file = platform::engine_settings_path(env);
    sources.system = system_report(sources.logs_dir);

    fs::path zip = out;
    if (zip.empty()) {
        const std::time_t now = std::time(nullptr);
        char name[64] = "opensupcom-logs.zip";
        if (const std::tm* t = std::localtime(&now))
            std::strftime(name, sizeof(name), "opensupcom-logs-%Y%m%d-%H%M%S.zip", t);
        zip = name;
    }
    const auto files = write_log_bundle(zip, sources);
    if (!files) {
        std::fprintf(stderr, "Couldn't write %s\n", zip.string().c_str());
        return 1;
    }
    std::error_code ec;
    const fs::path shown = fs::absolute(zip, ec);
    std::printf("Wrote %s (%d files): attach it to your bug report.\n",
                (ec ? zip : shown).string().c_str(), *files);
    return 0;
}

} // namespace osc::app
