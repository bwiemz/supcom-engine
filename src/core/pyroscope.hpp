#pragma once

// Pyroscope export (OSC_PYROSCOPE builds only): wall-clock samples of the
// main thread (service <app>) and the profiler's zones with their self time
// (service <app>-zones), posted as folded wall profiles to a Pyroscope server
// (or a relay) every window. Opt-in at
// configure time: a build without OSC_PYROSCOPE has none of this.

#include "core/types.hpp"

#include <map>
#include <string>

namespace osc::pyroscope {

struct Config {
    std::string url;                ///< http://host:port[/base]: the server or relay
    std::string app = "opensupcom"; ///< the services: <app> (samples) and <app>-zones
    std::map<std::string, std::string> labels;
    u32 sample_hz = 100; ///< main-thread samples per second
    u32 window_s = 10;   ///< seconds per posted profile
};

/// Starts sampling and posting; call it from the main thread (the one
/// sampled). False, with the reason logged, when the URL is not plain http.
bool start(const Config& config);
/// Whether start() succeeded and stop() has not run.
bool active();
/// A label on every profile from the next window on ("device", "map"...).
void set_label(const std::string& key, const std::string& value);
/// Posts what the current window holds and joins the threads.
void stop();
/// The profiler's hook: a zone closed with `self_us` of its own time (its
/// nested zones' time excluded); its path is names[0..depth), outermost first.
void record_zone(const char* const* names, u32 depth, f64 self_us);

} // namespace osc::pyroscope
