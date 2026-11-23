#include "core/test_status.hpp"

#include <atomic>
#include <mutex>

namespace osc::test_status {

namespace {

constexpr size_t kMaxKeptMessages = 50;

std::mutex g_mutex;
int g_count = 0;
std::vector<std::string> g_messages;
std::vector<std::string> g_allowed;
std::atomic<bool> g_count_lua{false};

} // namespace

void record_failure(std::string message) {
    std::lock_guard lock(g_mutex);
    for (const auto& text : g_allowed) {
        if (message.find(text) != std::string::npos) {
            return;
        }
    }
    ++g_count;
    if (g_messages.size() < kMaxKeptMessages) g_messages.push_back(std::move(message));
}

int failure_count() {
    std::lock_guard lock(g_mutex);
    return g_count;
}

std::vector<std::string> failure_messages() {
    std::lock_guard lock(g_mutex);
    return g_messages;
}

void allow_failure(std::string text) {
    std::lock_guard lock(g_mutex);
    g_allowed.push_back(std::move(text));
}

void reset() {
    std::lock_guard lock(g_mutex);
    g_count = 0;
    g_messages.clear();
    g_allowed.clear();
}

void set_count_lua_failures(bool enabled) { g_count_lua.store(enabled); }
bool count_lua_failures() { return g_count_lua.load(); }

} // namespace osc::test_status
