#pragma once

#include <string>
#include <unordered_map>
#include <variant>

namespace osc::core {

/// Simple key→value preferences backed by JSON file on disk.
/// Supports dot-notation keys (e.g., "options.graphics.quality").
/// FA's GetPreference/SetPreference map directly to get/set methods.
class Preferences {
public:
    void load(const std::string& path);
    void save(const std::string& path) const;

    std::string get_string(const std::string& key,
                           const std::string& default_val) const;
    float get_float(const std::string& key, float default_val) const;
    bool get_bool(const std::string& key, bool default_val) const;
    int get_int(const std::string& key, int default_val) const;

    void set_string(const std::string& key, const std::string& val);
    void set_float(const std::string& key, float val);
    void set_bool(const std::string& key, bool val);
    void set_int(const std::string& key, int val);

private:
    using Value = std::variant<std::string, float, bool, int>;
    std::unordered_map<std::string, Value> data_;
};

} // namespace osc::core
