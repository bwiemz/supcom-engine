#pragma once

// Pyroscope's folded ("collapsed") stack format and its /ingest URL: pure
// functions, in every build and under test; core/pyroscope.cpp (OSC_PYROSCOPE
// builds) is what samples and posts them.

#include "core/types.hpp"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace osc::pyroscope {

/// Where to post: http://host[:port][/base]. TLS and credentials are a
/// relay's job (Grafana Alloy's pyroscope.receive_http), not the game's.
struct HttpTarget {
    std::string host;
    u16 port = 80;
    std::string base_path; ///< "" or "/prefix" (no trailing slash)
};

inline bool parse_http_url(std::string_view url, HttpTarget& out) {
    constexpr std::string_view scheme = "http://";
    if (url.substr(0, scheme.size()) != scheme) return false;
    const std::string_view rest = url.substr(scheme.size());
    std::string_view hostport = rest;
    std::string_view path;
    if (const auto slash = rest.find('/'); slash != std::string_view::npos) {
        hostport = rest.substr(0, slash);
        path = rest.substr(slash);
    }
    HttpTarget t;
    if (const auto colon = hostport.rfind(':'); colon != std::string_view::npos) {
        const std::string_view digits = hostport.substr(colon + 1);
        if (digits.empty() || digits.size() > 5 ||
            !std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; }))
            return false;
        const unsigned long port = std::strtoul(std::string(digits).c_str(), nullptr, 10);
        if (port < 1 || port > 65535) return false;
        t.port = static_cast<u16>(port);
        hostport = hostport.substr(0, colon);
    }
    if (hostport.empty()) return false;
    t.host = std::string(hostport);
    while (!path.empty() && path.back() == '/') path.remove_suffix(1);
    t.base_path = std::string(path);
    out = std::move(t);
    return true;
}

/// A label key or value as Pyroscope takes it: letters, digits, "_.:/-".
inline std::string sanitize_label(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '.' || c == ':' || c == '/' ||
                        c == '-';
        if (!ok) c = '_';
    }
    return out;
}

inline std::string url_encode(std::string_view s) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size());
    for (const unsigned char c : s) {
        const bool unreserved = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
                                c == '~';
        if (unreserved) {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

/// The /ingest request for one window: `app{k=v,...}` over [from, until]
/// (UNIX seconds) in folded format, `sample_rate` samples per second.
inline std::string ingest_path(const HttpTarget& target, std::string_view app,
                               const std::map<std::string, std::string>& labels, i64 from,
                               i64 until, u32 sample_rate) {
    std::string name = sanitize_label(app);
    if (!labels.empty()) {
        name += '{';
        bool first = true;
        for (const auto& [key, value] : labels) {
            if (!first) name += ',';
            first = false;
            name += sanitize_label(key) + '=' + sanitize_label(value);
        }
        name += '}';
    }
    return target.base_path + "/ingest?name=" + url_encode(name) + "&from=" + std::to_string(from) +
           "&until=" + std::to_string(until) +
           "&format=folded&sampleRate=" + std::to_string(sample_rate) +
           "&units=samples&spyName=opensupcom";
}

/// Stacks ("root;child;leaf") with the value each accumulated over a window.
class FoldedProfile {
public:
    void add(std::string stack, u64 value) { counts_[std::move(stack)] += value; }
    bool empty() const { return counts_.empty(); }
    size_t size() const { return counts_.size(); }
    void clear() { counts_.clear(); }

    /// The request body: one "stack value" line per stack, sorted (stable
    /// output for tests and diffs).
    std::string text() const {
        std::vector<std::pair<std::string_view, u64>> lines;
        lines.reserve(counts_.size());
        for (const auto& [stack, value] : counts_) lines.emplace_back(stack, value);
        std::sort(lines.begin(), lines.end());
        std::string out;
        for (const auto& [stack, value] : lines) {
            out += stack;
            out += ' ';
            out += std::to_string(value);
            out += '\n';
        }
        return out;
    }

private:
    std::unordered_map<std::string, u64> counts_;
};

/// A frame as the flame graph should show it: the demangled function without
/// its return type and parameters, and without ';' (the stack separator).
inline std::string frame_name(std::string_view demangled) {
    std::string_view name = demangled;
    int depth = 0;
    for (size_t i = 0; i < name.size(); ++i) {
        const char c = name[i];
        if (c == '<') {
            ++depth;
        } else if (c == '>' && depth > 0) {
            --depth;
        } else if (c == '(' && depth == 0) {
            // "operator()" keeps its parentheses
            const bool call_op = i >= 8 && name.substr(i - 8, 8) == "operator" &&
                                 i + 1 < name.size() && name[i + 1] == ')';
            name = name.substr(0, call_op ? i + 2 : i);
            break;
        }
    }
    depth = 0;
    size_t space = std::string_view::npos;
    for (size_t i = 0; i < name.size(); ++i) {
        const char c = name[i];
        if (c == '<') ++depth;
        else if (c == '>' && depth > 0) --depth;
        else if (c == ' ' && depth == 0) space = i;
    }
    if (space != std::string_view::npos && space + 1 < name.size()) name = name.substr(space + 1);
    if (name.empty()) name = demangled;
    std::string out(name);
    for (char& c : out)
        if (c == ';' || c == '\n' || c == '\r') c = ',';
    return out;
}

} // namespace osc::pyroscope
