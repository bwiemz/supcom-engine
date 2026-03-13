#include "core/preferences.hpp"
#include <fstream>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace osc::core {

void Preferences::load(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        spdlog::debug("Preferences: no file at {}", path);
        return;
    }
    try {
        nlohmann::json j;
        f >> j;
        // Flatten nested JSON into dot-notation keys
        std::function<void(const nlohmann::json&, const std::string&)> flatten;
        flatten = [&](const nlohmann::json& obj, const std::string& prefix) {
            for (auto& [k, v] : obj.items()) {
                auto full_key = prefix.empty() ? k : prefix + "." + k;
                if (v.is_object()) {
                    flatten(v, full_key);
                } else if (v.is_string()) {
                    data_[full_key] = v.get<std::string>();
                } else if (v.is_boolean()) {
                    data_[full_key] = v.get<bool>();
                } else if (v.is_number_float()) {
                    data_[full_key] = v.get<float>();
                } else if (v.is_number_integer()) {
                    data_[full_key] = v.get<int>();
                }
            }
        };
        flatten(j, "");
        spdlog::info("Preferences: loaded {} keys from {}", data_.size(), path);
    } catch (const std::exception& e) {
        spdlog::warn("Preferences: failed to parse {}: {}", path, e.what());
    }
}

void Preferences::save(const std::string& path) const {
    nlohmann::json root;
    for (auto& [key, val] : data_) {
        // Split dot-notation into nested JSON
        auto* node = &root;
        size_t start = 0;
        while (true) {
            size_t dot = key.find('.', start);
            if (dot == std::string::npos) {
                auto leaf = key.substr(start);
                std::visit([&](auto&& v) { (*node)[leaf] = v; }, val);
                break;
            }
            node = &((*node)[key.substr(start, dot - start)]);
            start = dot + 1;
        }
    }
    std::ofstream f(path);
    if (f.is_open()) {
        f << root.dump(2);
        spdlog::debug("Preferences: saved {} keys to {}", data_.size(), path);
    }
}

std::string Preferences::get_string(const std::string& key,
                                     const std::string& def) const {
    auto it = data_.find(key);
    if (it == data_.end()) return def;
    if (auto* s = std::get_if<std::string>(&it->second)) return *s;
    return def;
}

float Preferences::get_float(const std::string& key, float def) const {
    auto it = data_.find(key);
    if (it == data_.end()) return def;
    if (auto* f = std::get_if<float>(&it->second)) return *f;
    if (auto* i = std::get_if<int>(&it->second)) return static_cast<float>(*i);
    return def;
}

bool Preferences::get_bool(const std::string& key, bool def) const {
    auto it = data_.find(key);
    if (it == data_.end()) return def;
    if (auto* b = std::get_if<bool>(&it->second)) return *b;
    return def;
}

int Preferences::get_int(const std::string& key, int def) const {
    auto it = data_.find(key);
    if (it == data_.end()) return def;
    if (auto* i = std::get_if<int>(&it->second)) return *i;
    if (auto* f = std::get_if<float>(&it->second)) return static_cast<int>(*f);
    return def;
}

void Preferences::set_string(const std::string& key, const std::string& val) {
    data_[key] = val;
}

void Preferences::set_float(const std::string& key, float val) {
    data_[key] = val;
}

void Preferences::set_bool(const std::string& key, bool val) {
    data_[key] = val;
}

void Preferences::set_int(const std::string& key, int val) {
    data_[key] = val;
}

} // namespace osc::core
