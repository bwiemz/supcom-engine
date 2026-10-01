#include "renderer/vk_cmd.hpp"

namespace osc::renderer {

CommandCounts& command_counts() {
    static CommandCounts counts;
    return counts;
}

CommandCounts take_command_counts() {
    CommandCounts& counts = command_counts();
    const CommandCounts taken = counts;
    counts = {};
    return taken;
}

} // namespace osc::renderer
