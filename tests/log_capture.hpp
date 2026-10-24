#pragma once

// What the engine logs while one of these lives: the default logger's lines,
// to count the warnings a test expects.

#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <cstddef>
#include <memory>
#include <sstream>
#include <string>

namespace osc::test {

struct LogCapture {
    std::ostringstream text;
    std::shared_ptr<spdlog::logger> previous = spdlog::default_logger();
    LogCapture() {
        spdlog::set_default_logger(std::make_shared<spdlog::logger>(
            "capture", std::make_shared<spdlog::sinks::ostream_sink_st>(text)));
    }
    ~LogCapture() { spdlog::set_default_logger(previous); }
    LogCapture(const LogCapture&) = delete;
    LogCapture& operator=(const LogCapture&) = delete;
    /// How many times `part` occurs in what was logged.
    size_t count(const std::string& part) const {
        const std::string all = text.str();
        size_t n = 0;
        for (size_t at = all.find(part); at != std::string::npos; at = all.find(part, at + 1)) ++n;
        return n;
    }
};

} // namespace osc::test
