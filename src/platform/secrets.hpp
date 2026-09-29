#pragma once

// Secrets the engine keeps (M208c-c: the key that signs saved games'
// snapshots): random bytes fit for a key, and a file only its owner reads.

#include <cstddef>
#include <filesystem>

namespace osc::platform {

/// Fill `out` with `size` bytes from the OS's cryptographic random source
/// (getentropy; BCryptGenRandom on Windows). False if it couldn't give them
/// all: nothing weaker is used instead.
bool secure_random(void* out, std::size_t size);

/// Write `data` to `path` so that only its owner can read it, replacing any
/// file there whole: written beside it, then renamed over it. On POSIX the
/// file is created 0600; on Windows it takes its folder's permissions (a
/// user profile's folder is its user's). False if it couldn't be written.
bool write_private_file(const std::filesystem::path& path, const void* data, std::size_t size);

} // namespace osc::platform
