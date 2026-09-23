#include "vfs/path_utils.hpp"

#include <algorithm>
#include <cctype>
#include <string>

namespace osc::vfs {

namespace fs = std::filesystem;

namespace {

char lower(char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), lower);
    return s;
}

bool iequals(std::string_view a, std::string_view b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(),
                      [](char x, char y) { return lower(x) == lower(y); });
}

/// Find the entry in `dir` whose name equals `name` ignoring case.
std::optional<fs::path> find_entry_ci(const fs::path& dir, std::string_view name) {
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end;
         it.increment(ec)) {
        if (iequals(it->path().filename().string(), name)) {
            return it->path();
        }
    }
    return std::nullopt;
}

} // namespace

bool wildcard_match(std::string_view pattern, std::string_view name) {
    // Iterative matcher with single-star backtracking: O(|p| * |n|) worst case.
    size_t p = 0;
    size_t n = 0;
    size_t star_p = std::string_view::npos;
    size_t star_n = 0;
    while (n < name.size()) {
        if (p < pattern.size() &&
            (pattern[p] == '?' || lower(pattern[p]) == lower(name[n]))) {
            ++p;
            ++n;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star_p = p++;
            star_n = n;
        } else if (star_p != std::string_view::npos) {
            p = star_p + 1;
            n = ++star_n;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

bool has_wildcard(std::string_view s) {
    return s.find_first_of("*?") != std::string_view::npos;
}

std::optional<fs::path> resolve_case_insensitive(const fs::path& path) {
    std::error_code ec;
    if (fs::exists(path, ec)) return path;

    fs::path current = path.root_path();
    for (const auto& part : path.relative_path()) {
        const std::string name = part.string();
        if (name.empty() || name == ".") continue;
        if (name == "..") {
            if (current.has_relative_path() && current.filename() != "..") {
                current = current.parent_path();
            } else if (!current.has_root_path()) {
                current /= ".."; // relative path climbing above its start
            }
            continue;
        }
        fs::path exact = current.empty() ? fs::path(name) : current / name;
        if (fs::exists(exact, ec)) {
            current = std::move(exact);
            continue;
        }
        auto match = find_entry_ci(current.empty() ? fs::path(".") : current, name);
        if (!match) return std::nullopt;
        current = current.empty() ? match->filename() : *match;
    }
    return current;
}

std::vector<fs::path> expand_glob(const fs::path& pattern) {
    std::vector<fs::path> out;
    const std::string leaf = pattern.filename().string();
    if (!has_wildcard(leaf)) {
        if (auto resolved = resolve_case_insensitive(pattern)) {
            out.push_back(std::move(*resolved));
        }
        return out;
    }

    auto dir = resolve_case_insensitive(pattern.parent_path());
    std::error_code ec;
    if (!dir || !fs::is_directory(*dir, ec)) return out;

    for (fs::directory_iterator it(*dir, ec), end; !ec && it != end;
         it.increment(ec)) {
        if (wildcard_match(leaf, it->path().filename().string())) {
            out.push_back(it->path());
        }
    }
    std::sort(out.begin(), out.end(), [](const fs::path& a, const fs::path& b) {
        return to_lower(a.filename().string()) < to_lower(b.filename().string());
    });
    return out;
}

} // namespace osc::vfs
