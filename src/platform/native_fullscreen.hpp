#pragma once

struct GLFWwindow;

namespace osc::platform {

bool native_fullscreen(GLFWwindow* window);
/// Between a Space transition's will- and did- notifications.
bool native_fullscreen_moving(GLFWwindow* window);
void toggle_native_fullscreen(GLFWwindow* window);

} // namespace osc::platform
