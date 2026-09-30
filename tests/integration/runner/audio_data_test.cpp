#include "audio_data_test.hpp"

#include "audio/xact/bank_registry.hpp"
#include "audio/xact/global_settings.hpp"
#include "core/test_status.hpp"

#include <spdlog/spdlog.h>

#include <cmath>
#include <set>
#include <string>

namespace osc::test {

void run_audio_data_test(const std::filesystem::path& sounds_dir) {
    spdlog::info("=== AUDIO DATA TEST (M188) ===");
    audio::xact::BankRegistry registry(sounds_dir);
    const auto* gs = registry.global_settings();
    if (!gs || gs->categories.empty()) {
        test_status::fail("[FAIL] no XACT global settings (.xgs) in {}", sounds_dir.string());
        return;
    }
    spdlog::info("[PASS] global settings: {} categories, {} variables, {} RPC curves",
                 gs->categories.size(), gs->variables.size(), gs->rpcs.size());

    // Each positional cue's Angle (M216b): cue-scoped, and read by a curve
    // that is ~0 dB straight below the listener and -20 dB at the horizon,
    // so the engine's "degrees off straight up" is the quantity it means.
    {
        const int angle = gs->find_variable("Angle");
        const audio::xact::RpcCurve* curve = nullptr;
        for (const auto& rpc : gs->rpcs)
            if (angle >= 0 && rpc.variable == static_cast<u16>(angle)) curve = &rpc;
        if (angle < 0 || gs->variables[static_cast<size_t>(angle)].global() || !curve)
            test_status::fail("[FAIL] no cue-scoped Angle variable with a curve");
        else if (std::abs(curve->evaluate(180.0f)) < 60.0f && curve->evaluate(90.0f) < -1900.0f)
            spdlog::info("[PASS] Angle's curve: {:.0f} mB below, {:.0f} mB level",
                         curve->evaluate(180.0f), curve->evaluate(90.0f));
        else
            test_status::fail("[FAIL] Angle's curve: {:.0f} mB below, {:.0f} mB level",
                              curve->evaluate(180.0f), curve->evaluate(90.0f));
    }

    // The voice banks (EVA, campaign and briefing VO, the movies' voices)
    // in sounds/Voice/US and its tutorials, as AudioSetLanguage('us') loads.
    const auto voice =
        audio::xact::child_any_case(audio::xact::child_any_case(sounds_dir, "voice"), "us");
    if (!voice.empty()) {
        registry.add_directory(voice);
        if (const auto tutorials = audio::xact::child_any_case(voice, "tutorials");
            !tutorials.empty())
            registry.add_directory(tutorials);
        if (registry.sound_bank("XGG") && registry.sound_bank("X_FMV"))
            spdlog::info("[PASS] the voice banks load (XGG, X_FMV)");
        else test_status::fail("[FAIL] no XGG or X_FMV voice bank in {}", voice.string());
    }

    size_t banks = 0, cues = 0, waves = 0;
    int reported = 0;
    auto problem = [&](const std::string& what) {
        if (reported++ < 25) spdlog::error("  {}", what);
        test_status::record_failure(what);
    };
    std::set<const audio::XwbParser*> read_back; // one wave per wave bank from disk

    for (const auto& name : registry.sound_bank_names()) {
        const auto* sb = registry.sound_bank(name);
        if (!sb) {
            problem("sound bank " + name + " does not parse");
            continue;
        }
        ++banks;
        for (const auto& cue : sb->cues) {
            ++cues;
            const std::string where = name + "/" + cue.name;
            const auto& sound = sb->sounds[cue.sound];
            if (sound.category >= gs->categories.size())
                problem(where + ": category " + std::to_string(sound.category) + " missing");
            for (u32 code : sound.rpc_codes)
                if (!gs->rpc(code)) problem(where + ": RPC curve " + std::to_string(code) + " missing");
            size_t cue_waves = 0;
            for (const auto& track : sound.tracks) {
                for (u32 code : track.rpc_codes)
                    if (!gs->rpc(code)) problem(where + ": track RPC " + std::to_string(code) + " missing");
                for (const auto& play : track.plays) {
                    for (const auto& choice : play.waves) {
                        ++cue_waves;
                        const auto wave = registry.resolve(*sb, choice);
                        if (!wave) {
                            problem(where + ": wave " + std::to_string(choice.wave) + " of bank '" +
                                    sb->wave_banks[choice.bank] + "' not found");
                            continue;
                        }
                        ++waves;
                        const auto& info = wave->bank->entry(wave->index);
                        if ((info.format_tag != 0 && info.format_tag != 2) || info.data_length == 0 ||
                            info.channels == 0 || info.sample_rate == 0) {
                            problem(where + ": unplayable wave format");
                            continue;
                        }
                        if (read_back.insert(wave->bank).second &&
                            wave->bank->read_wave_data(wave->index).size() != info.data_length)
                            problem(where + ": wave data does not read back from " +
                                    wave->bank->bank_name());
                    }
                }
            }
            if (cue_waves == 0) problem(where + ": plays no wave");
        }
    }
    // Only Music's two game tracks pick a new wave for each loop (M216b).
    std::set<std::string> repicking;
    for (const auto& name : registry.sound_bank_names()) {
        const auto* sb = registry.sound_bank(name);
        if (!sb) continue;
        for (const auto& cue : sb->cues) {
            if (cue.sound >= sb->sounds.size()) continue;
            for (const auto& track : sb->sounds[cue.sound].tracks)
                for (const auto& play : track.plays)
                    if (play.new_variation_on_loop) repicking.insert(name + "/" + cue.name);
        }
    }
    if (repicking == std::set<std::string>{"Music/Base_Building", "Music/Battle"})
        spdlog::info("[PASS] Music's Base_Building and Battle pick a new track each loop");
    else
        problem("the cues that pick a new wave each loop are not Music's two: " +
                std::to_string(repicking.size()));
    if (reported > 25) spdlog::error("  ... {} more", reported - 25);
    spdlog::info("Audio data: {} sound banks, {} cues, {} wave references ({} wave banks read back)",
                 banks, cues, waves, read_back.size());
    if (banks == 0) test_status::fail("[FAIL] no sound banks in {}", sounds_dir.string());
    else if (reported == 0) spdlog::info("[PASS] every cue resolves to playable waves");
}

} // namespace osc::test
