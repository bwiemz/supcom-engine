#include "sim/build_info.hpp"

#include "core/version.hpp"

namespace osc::sim {

const char* build_id() {
    return core::build_id();
}

} // namespace osc::sim
