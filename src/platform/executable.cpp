#include "platform/executable.hpp"

#include <cstring>
#include <string>
#include <system_error>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#elif defined(__linux__)
#include <elf.h>
#include <link.h>
#endif

namespace osc::platform {

#ifdef _WIN32

std::filesystem::path executable_path() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (n == 0) return {};
        if (n < path.size()) {
            path.resize(n);
            return path;
        }
        path.resize(path.size() * 2); // truncated: a longer path
    }
}

std::vector<std::uint8_t> executable_build_id() {
    return {};
}

#elif defined(__linux__)

std::filesystem::path executable_path() {
    std::error_code ec;
    std::filesystem::path path = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::filesystem::path() : path;
}

namespace {

/// The NT_GNU_BUILD_ID note among the main program's PT_NOTE segments.
int find_build_id(dl_phdr_info* info, size_t /*size*/, void* data) {
    // The first object dl_iterate_phdr visits is the main program
    auto* out = static_cast<std::vector<std::uint8_t>*>(data);
    for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr) & ph = info->dlpi_phdr[i];
        if (ph.p_type != PT_NOTE) continue;
        // dl_iterate_phdr gives the load address as an integer (ElfW(Addr)):
        // no pointer to derive the segment's from.
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        const auto* p = reinterpret_cast<const unsigned char*>(info->dlpi_addr + ph.p_vaddr);
        const unsigned char* end = p + ph.p_memsz;
        // Notes pad to their segment's alignment: 4, or 8 for some (GNU
        // property notes)
        const size_t align = ph.p_align == 8 ? 8 : 4;
        const auto padded = [align](size_t n) { return (n + align - 1) & ~(align - 1); };
        while (p + sizeof(ElfW(Nhdr)) <= end) {
            ElfW(Nhdr) note{};
            std::memcpy(&note, p, sizeof(note));
            const unsigned char* name = p + sizeof(note);
            const unsigned char* desc = name + padded(note.n_namesz);
            if (desc > end || note.n_descsz > static_cast<size_t>(end - desc)) break;
            if (note.n_type == NT_GNU_BUILD_ID && note.n_namesz == 4 &&
                std::memcmp(name, "GNU", 4) == 0) {
                out->assign(desc, desc + note.n_descsz);
                return 1;
            }
            p = desc + padded(note.n_descsz);
        }
    }
    return 1; // only the main program
}

} // namespace

std::vector<std::uint8_t> executable_build_id() {
    std::vector<std::uint8_t> id;
    dl_iterate_phdr(find_build_id, &id);
    return id;
}

#else

std::filesystem::path executable_path() {
    return {};
}

std::vector<std::uint8_t> executable_build_id() {
    return {};
}

#endif

} // namespace osc::platform
