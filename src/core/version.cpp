#include "core/version.hpp"

#include "osc_revision.hpp" // generated at every build (cmake/OscRevision.cmake)

#ifndef OSC_VERSION
#error "OSC_VERSION is defined by src/core/CMakeLists.txt"
#endif

namespace osc::core {

const char* version() {
    return OSC_VERSION;
}

const char* revision() {
    return OSC_REVISION;
}

const char* build_id() {
    return OSC_VERSION "-" OSC_REVISION;
}

const char* version_line() {
    return "OpenSupCom " OSC_VERSION " (" OSC_REVISION ")";
}

} // namespace osc::core
