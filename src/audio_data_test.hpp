#pragma once

#include <filesystem>

namespace osc::test {

/// --audio-data-test: every sound bank in `sounds_dir` parses, and every
/// cue plays something real -- its sound's category and RPC curves exist
/// in the global settings, and each wave it can start resolves (through
/// the wave bank's internal name) to a PCM or ADPCM wave with data, which
/// reads back from disk. Failures are recorded in test_status.
void run_audio_data_test(const std::filesystem::path& sounds_dir);

} // namespace osc::test
