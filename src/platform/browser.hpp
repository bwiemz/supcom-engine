#pragma once

#include <string>

namespace osc::platform {

/// Open `url` with the user's default handler, as Moho's OpenURL does
/// (ShellExecuteW "open" on Windows; `open` on macOS, `xdg-open` elsewhere),
/// started directly, never through a shell. Returns whether the handler
/// started; it runs on without the engine waiting for it.
bool open_url(const std::string& url);

} // namespace osc::platform
