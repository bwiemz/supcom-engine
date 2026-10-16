#pragma once

// A string kept once for the whole run (interned): copying one copies a
// pointer. For the few names that recur across thousands of entities --
// blueprint ids, mesh paths -- which the per-tick world snapshot copies for
// every entity (sim::capture_world): as std::strings that was a third of a
// tick's time in an eight-AI match.

#include <string>
#include <string_view>

namespace osc {

class InternedString {
public:
    InternedString() : s_(&empty_string()) {}
    explicit InternedString(std::string_view text) : s_(&intern(text)) {}

    const std::string& str() const { return *s_; }
    operator const std::string&() const { return *s_; } // NOLINT(google-explicit-constructor)
    const char* c_str() const { return s_->c_str(); }
    size_t size() const { return s_->size(); }
    bool empty() const { return s_->empty(); }

    friend bool operator==(const InternedString& a, const InternedString& b) {
        return a.s_ == b.s_;
    }
    friend bool operator==(const InternedString& a, std::string_view b) { return *a.s_ == b; }
    friend bool operator==(const InternedString& a, const char* b) { return *a.s_ == b; }
    friend bool operator==(const InternedString& a, const std::string& b) { return *a.s_ == b; }

private:
    /// The one copy of `text`, made on first sight and kept until exit (so
    /// a snapshot can outlive the entity it was taken from). Thread-safe.
    static const std::string& intern(std::string_view text);
    static const std::string& empty_string();

    const std::string* s_;
};

/// For fmt and spdlog: an interned string prints as its text.
inline std::string_view format_as(const InternedString& s) { return s.str(); }

} // namespace osc
