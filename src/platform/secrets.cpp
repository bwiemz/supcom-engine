#include "platform/secrets.hpp"

#include <system_error>

#ifdef _WIN32
#include <windows.h>
// (after windows.h)
#include <bcrypt.h>

#include <fstream>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace osc::platform {

bool secure_random(void* out, std::size_t size) {
#ifdef _WIN32
    auto* p = static_cast<unsigned char*>(out);
    while (size > 0) {
        const ULONG n = size > 0x10000000 ? 0x10000000 : static_cast<ULONG>(size);
        if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, p, n, BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
            return false;
        p += n;
        size -= n;
    }
    return true;
#else
    auto* p = static_cast<unsigned char*>(out);
    while (size > 0) {
        const std::size_t n = size > 256 ? 256 : size; // getentropy's most
        if (getentropy(p, n) != 0) return false;
        p += n;
        size -= n;
    }
    return true;
#endif
}

bool write_private_file(const std::filesystem::path& path, const void* data, std::size_t size) {
    std::filesystem::path part = path;
    part += ".part";
    std::error_code ec;
#ifdef _WIN32
    {
        std::ofstream out(part, std::ios::binary | std::ios::trunc);
        if (!out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size)))
            return false;
    }
#else
    // Made 0600 from the start: a leftover .part of other permissions goes
    // first (O_TRUNC would keep its mode).
    ::unlink(part.c_str());
    const int fd = ::open(part.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    const auto* p = static_cast<const char*>(data);
    std::size_t left = size;
    while (left > 0) {
        const ssize_t n = ::write(fd, p, left);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            ::close(fd);
            ::unlink(part.c_str());
            return false;
        }
        p += n;
        left -= static_cast<std::size_t>(n);
    }
    if (::fsync(fd) != 0 || ::close(fd) != 0) {
        ::unlink(part.c_str());
        return false;
    }
#endif
    std::filesystem::rename(part, path, ec);
    if (ec) {
        std::filesystem::remove(part, ec);
        return false;
    }
    return true;
}

} // namespace osc::platform
