// The first run's native dialogs (M228a), through portable-file-dialogs:
// Windows' own dialogs, or on Linux a desktop helper program.

#include "platform/first_run.hpp"

#include <portable-file-dialogs.h>

#include <algorithm>
#include <cstdlib>
#include <string>
#include <string_view>
#include <system_error>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace osc::platform {

namespace fs = std::filesystem;

namespace {

#ifndef _WIN32
/// Whether `program` is an executable on PATH.
bool on_path(std::string_view program) {
    const char* path = std::getenv("PATH");
    if (!path) return false;
    const std::string_view dirs(path);
    for (size_t start = 0; start <= dirs.size();) {
        const size_t end = std::min(dirs.find(':', start), dirs.size());
        const fs::path dir(dirs.substr(start, end - start));
        std::error_code ec;
        const fs::path file = dir / program;
        if (!dir.empty() && fs::is_regular_file(file, ec) && ::access(file.c_str(), X_OK) == 0)
            return true;
        start = end + 1;
    }
    return false;
}

/// Whether pfd can show a dialog here. Without one of its helpers it would
/// "show" it by running echo, and a folder pick would return echo's
/// arguments as the folder.
bool dialogs_available() {
    const bool display = std::getenv("DISPLAY") || std::getenv("WAYLAND_DISPLAY");
    if (!display) return false;
    for (const char* helper : {"zenity", "matedialog", "qarma", "kdialog"})
        if (on_path(helper)) return true;
    return false;
}
#endif

class NativePrompter final : public Prompter {
public:
    bool ask(const std::string& title, const std::string& text) override {
        return pfd::message(title, text, pfd::choice::ok_cancel, pfd::icon::question).result() ==
               pfd::button::ok;
    }

    std::optional<fs::path> pick_folder(const std::string& title, const fs::path& start) override {
        std::string folder = pfd::select_folder(title, start.string()).result();
        if (folder.empty()) return std::nullopt;
        return fs::path(folder);
    }
};

} // namespace

std::unique_ptr<Prompter> make_native_prompter() {
#ifndef _WIN32
    if (!dialogs_available()) return nullptr;
#endif
    return std::make_unique<NativePrompter>();
}

} // namespace osc::platform
