#include "platform/first_run.hpp"

#include "platform/engine_settings.hpp"
#include "platform/game_install.hpp"

namespace osc::platform {

namespace fs = std::filesystem;

namespace {

constexpr const char* kTitle = "OpenSupCom: where is Forged Alliance?";

} // namespace

FirstRunResult ask_for_fa_install(Prompter& prompter, const fs::path& settings_file,
                                  const fs::path& start) {
    FirstRunResult result;
    if (!prompter.ask(kTitle,
                      "OpenSupCom plays Supreme Commander: Forged Alliance from your own copy "
                      "of the game, and couldn't find one.\n\n"
                      "Choose the folder Forged Alliance is installed in: the one holding "
                      "its \"bin\" and \"gamedata\" folders."))
        return result;
    fs::path from = start;
    for (;;) {
        const auto picked = prompter.pick_folder(kTitle, from);
        if (!picked) return result;
        if (auto fa = resolve_fa_folder(*picked)) {
            EngineSettings settings = load_engine_settings(settings_file);
            settings.fa_path = *fa;
            result.fa_path = *fa;
            result.saved = save_engine_settings(settings_file, settings);
            return result;
        }
        const std::string problem = fa_install_problem(*picked).value_or("it doesn't hold FA");
        if (!prompter.ask(kTitle, picked->string() + " isn't a Forged Alliance install: " +
                                      problem + ".\n\nChoose another folder?"))
            return result;
        from = *picked;
    }
}

} // namespace osc::platform
