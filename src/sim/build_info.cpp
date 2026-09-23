#include "sim/build_info.hpp"

#ifndef OSC_BUILD_ID
#define OSC_BUILD_ID "unknown"
#endif

namespace osc::sim {

const char* build_id() {
    return OSC_BUILD_ID;
}

} // namespace osc::sim
