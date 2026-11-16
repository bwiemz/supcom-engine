#pragma once

namespace osc::audio {
class SoundManager;
}

namespace osc::test {

/// --audio-data-test: the run's sound banks, as the engine loaded them from
/// the VFS's /sounds (FA's, and any mod's or map's the init file mounted
/// there) with the US voice banks: every sound bank parses, and every cue
/// plays something real -- its sound's category and RPC curves exist in
/// the global settings, and each wave it can start resolves (through the
/// wave bank's internal name) to a PCM or ADPCM wave with data, which reads
/// back from its bank. Logs the banks' names. Failures are recorded in
/// test_status.
void run_audio_data_test(audio::SoundManager& sound);

} // namespace osc::test
