#pragma once

// The first run (M228a): when no FA install is found, a windowed start asks
// the player where it is, instead of exiting with a command-line hint.

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace osc::platform {

/// What the first run asks the player: native dialogs in the game
/// (make_native_prompter), scripted answers in tests.
class Prompter {
public:
    virtual ~Prompter() = default;
    Prompter() = default;
    Prompter(const Prompter&) = delete;
    Prompter& operator=(const Prompter&) = delete;
    Prompter(Prompter&&) = delete;
    Prompter& operator=(Prompter&&) = delete;

    /// An OK/Cancel question: true for OK.
    virtual bool ask(const std::string& title, const std::string& text) = 0;
    /// A folder the player picks, starting at `start`: nullopt if cancelled.
    virtual std::optional<std::filesystem::path>
    pick_folder(const std::string& title, const std::filesystem::path& start) = 0;
};

/// Native dialogs: Windows' own, or on Linux zenity, matedialog, qarma or
/// kdialog, whichever is installed. Null when there is none to show them
/// with (no helper, or no display), so the caller falls back to a message.
std::unique_ptr<Prompter> make_native_prompter();

struct FirstRunResult {
    /// The FA folder the player picked; nullopt if they cancelled.
    std::optional<std::filesystem::path> fa_path;
    /// Whether it was saved (else the next start asks again).
    bool saved = false;
};

/// Ask the player where FA is installed, until they pick a folder holding it
/// (or FA's bin or gamedata folder inside it) or cancel. The folder is saved
/// in the engine's settings file, `settings_file`.
FirstRunResult ask_for_fa_install(Prompter& prompter, const std::filesystem::path& settings_file,
                                  const std::filesystem::path& start);

} // namespace osc::platform
