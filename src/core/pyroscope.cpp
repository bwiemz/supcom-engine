// Pyroscope export: see pyroscope.hpp. Two threads beside the game's:
//
//  - the sampler sends the main thread SIGPROF `sample_hz` times a second;
//    the handler records the thread's stack (addresses only, async-signal-
//    safe), and the sampler files it under those addresses;
//  - the pusher, every `window_s` seconds, symbolicates the window's stacks
//    (dladdr, cached), folds them with the zones the profiler reported, and
//    POSTs both to <url>/ingest over plain HTTP (sockets, no library).
//
// Sampling is wall-clock: a main thread waiting for the GPU is sampled in
// the wait, which is what a frame budget wants to see.

#include "core/pyroscope.hpp"

#include "core/pyroscope_folded.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <cxxabi.h>
#include <dlfcn.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#if defined(__APPLE__) && defined(__aarch64__)
#include <sys/ucontext.h>
#define OSC_PYROSCOPE_FP_WALK 1
#else
#include <execinfo.h>
#define OSC_PYROSCOPE_FP_WALK 0
#endif

namespace osc::pyroscope {
namespace {

using steady = std::chrono::steady_clock;
using wall = std::chrono::system_clock;

constexpr int kMaxFrames = 128;

// The sample in flight: the SIGPROF handler fills it on the main thread and
// publishes the count; the sampler thread reads it then.
uintptr_t g_frames[kMaxFrames];
std::atomic<int> g_frame_count{-1}; // -1: none pending
uintptr_t g_stack_lo = 0;
uintptr_t g_stack_hi = 0;

void on_sigprof(int, siginfo_t*, void* ctx) {
    int n = 0;
#if OSC_PYROSCOPE_FP_WALK
    // arm64 keeps frame pointers (its ABI requires them), so walking them
    // is async-signal-safe: no unwinder, no locks.
    const auto* uc = static_cast<const ucontext_t*>(ctx);
    const auto& ss = uc->uc_mcontext->__ss;
#ifdef __darwin_arm_thread_state64_get_pc
    const auto pc = static_cast<uintptr_t>(__darwin_arm_thread_state64_get_pc(ss));
    const auto lr = static_cast<uintptr_t>(__darwin_arm_thread_state64_get_lr(ss));
    auto fp = static_cast<uintptr_t>(__darwin_arm_thread_state64_get_fp(ss));
#else
    const auto pc = static_cast<uintptr_t>(ss.__pc);
    const auto lr = static_cast<uintptr_t>(ss.__lr);
    auto fp = static_cast<uintptr_t>(ss.__fp);
#endif
    g_frames[n++] = pc;
    bool first = true;
    while (n < kMaxFrames - 1 && fp >= g_stack_lo && fp + 16 <= g_stack_hi && (fp & 0xF) == 0) {
        const auto* frame = reinterpret_cast<const uintptr_t*>(fp);
        const uintptr_t next = frame[0];
        const uintptr_t ret = frame[1];
        if (first) {
            // A leaf's (or a prologue's) caller is only in lr; a frame past
            // its prologue saved lr as ret. Keep an lr that differs: the
            // pusher drops it when it names the sampled function itself.
            if (lr != 0 && lr != ret) g_frames[n++] = lr;
            first = false;
        }
        if (ret == 0) break;
        g_frames[n++] = ret;
        if (next <= fp) break;
        fp = next;
    }
#else
    (void)ctx;
    void* buf[kMaxFrames];
    n = backtrace(buf, kMaxFrames);
    for (int i = 0; i < n; ++i) g_frames[i] = reinterpret_cast<uintptr_t>(buf[i]);
#endif
    g_frame_count.store(n, std::memory_order_release);
}

struct State {
    Config cfg;
    HttpTarget target;
    pthread_t main_thread{};
    std::atomic<bool> running{false};
    std::thread sampler;
    std::thread pusher;

    std::mutex mu; // guards the window below
    std::condition_variable cv;
    std::unordered_map<std::string, u64> samples; // key: the stack's addresses, as bytes
    FoldedProfile zones;
    std::map<std::string, std::string> labels;
    u64 dropped = 0;
    wall::time_point window_from;

    // The pusher's own
    std::unordered_map<uintptr_t, std::string> symbols;
    bool posted = false;
    bool failing = false;
};

// Left allocated when the game exits without stop(): the threads must not
// find it gone.
State* g_state = nullptr;

void sampler_main(State& s) {
#ifdef __APPLE__
    pthread_setname_np("pyroscope sampler");
#endif
    const auto period = std::chrono::microseconds(1000000 / std::max<u32>(s.cfg.sample_hz, 1));
    auto next = steady::now();
    while (s.running.load(std::memory_order_relaxed)) {
        next += period;
        std::this_thread::sleep_until(next);
        if (!s.running.load(std::memory_order_relaxed)) break;
        g_frame_count.store(-1, std::memory_order_relaxed);
        if (pthread_kill(s.main_thread, SIGPROF) != 0) break;
        const auto deadline = steady::now() + std::chrono::milliseconds(20);
        int n = -1;
        while ((n = g_frame_count.load(std::memory_order_acquire)) < 0 && steady::now() < deadline)
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        std::lock_guard lock(s.mu);
        if (n < 0) {
            ++s.dropped;
            continue;
        }
        s.samples[std::string(reinterpret_cast<const char*>(g_frames),
                              sizeof(uintptr_t) * static_cast<size_t>(n))] += 1;
    }
}

const std::string& symbol_for(State& s, uintptr_t addr, bool return_address) {
    if (const auto it = s.symbols.find(addr); it != s.symbols.end()) return it->second;
    // A return address is after the call: look up the instruction before it
    const uintptr_t lookup = return_address ? addr - 1 : addr;
    Dl_info info{};
    std::string name;
    // dladdr names the nearest preceding symbol, which for an address in a
    // function without one (a stripped local) is some other function: a name
    // from more than 64 KB back is taken as none.
    constexpr uintptr_t kFarthestSymbol = 64 * 1024;
    if (dladdr(reinterpret_cast<void*>(lookup), &info) != 0 && info.dli_sname &&
        lookup - reinterpret_cast<uintptr_t>(info.dli_saddr) < kFarthestSymbol) {
        int status = 0;
        char* demangled = abi::__cxa_demangle(info.dli_sname, nullptr, nullptr, &status);
        name = frame_name(status == 0 && demangled ? demangled : info.dli_sname);
        std::free(demangled);
    } else if (info.dli_fname) {
        const char* base = std::strrchr(info.dli_fname, '/');
        name = fmt::format("{}+0x{:x}", base ? base + 1 : info.dli_fname,
                           lookup - reinterpret_cast<uintptr_t>(info.dli_fbase));
    } else {
        name = fmt::format("0x{:x}", addr);
    }
    return s.symbols.emplace(addr, std::move(name)).first->second;
}

bool http_post(const HttpTarget& target, const std::string& path, const std::string& body,
               std::string& error) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* found = nullptr;
    if (const int e =
            getaddrinfo(target.host.c_str(), std::to_string(target.port).c_str(), &hints, &found);
        e != 0) {
        error = gai_strerror(e);
        return false;
    }
    int fd = -1;
    for (const addrinfo* ai = found; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        const timeval timeout{2, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
#ifdef SO_NOSIGPIPE
        const int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(found);
    if (fd < 0) {
        error = std::strerror(errno);
        return false;
    }
#ifdef MSG_NOSIGNAL
    constexpr int send_flags = MSG_NOSIGNAL;
#else
    constexpr int send_flags = 0;
#endif
    const std::string request = fmt::format("POST {} HTTP/1.1\r\nHost: {}:{}\r\nContent-Type: "
                                            "text/plain\r\nContent-Length: {}\r\nConnection: "
                                            "close\r\n\r\n{}",
                                            path, target.host, target.port, body.size(), body);
    size_t sent = 0;
    while (sent < request.size()) {
        const ssize_t n = send(fd, request.data() + sent, request.size() - sent, send_flags);
        if (n <= 0) {
            error = fmt::format("send: {}", std::strerror(errno));
            close(fd);
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    std::string head;
    char buf[1024];
    while (head.find("\r\n\r\n") == std::string::npos && head.size() < 8192) {
        const ssize_t n = recv(fd, buf, sizeof buf, 0);
        if (n <= 0) break;
        head.append(buf, static_cast<size_t>(n));
    }
    close(fd);
    const auto line_end = head.find("\r\n");
    const std::string status = head.substr(0, line_end); // "HTTP/1.1 200 OK"
    const auto space = status.find(' ');
    const int code = space == std::string::npos ? 0 : std::atoi(status.c_str() + space + 1);
    if (code < 200 || code > 299) {
        error = status.empty() ? "no response" : status;
        return false;
    }
    return true;
}

// The last window may post after the log has shut down
template <typename... Args>
void log(spdlog::level::level_enum level, fmt::format_string<Args...> format, Args&&... args) {
    if (spdlog::logger* logger = spdlog::default_logger_raw())
        logger->log(level, format, std::forward<Args>(args)...);
}

void post(State& s, const std::string& app, const std::map<std::string, std::string>& labels,
          i64 from, i64 until, u32 sample_rate, const FoldedProfile& profile) {
    if (profile.empty()) return;
    std::string error;
    if (http_post(s.target, ingest_path(s.target, app, labels, from, until, sample_rate),
                  profile.text(), error)) {
        if (!s.posted) log(spdlog::level::info, "Pyroscope: posting {} to {}", app, s.cfg.url);
        s.posted = true;
        s.failing = false;
    } else if (!s.failing) {
        log(spdlog::level::warn, "Pyroscope: {} not posted to {}: {}", app, s.cfg.url, error);
        s.failing = true;
    }
}

/// Posts the window so far and starts the next.
void flush(State& s) {
    std::unordered_map<std::string, u64> samples;
    FoldedProfile zones;
    std::map<std::string, std::string> labels;
    wall::time_point from;
    u64 dropped = 0;
    {
        std::lock_guard lock(s.mu);
        samples.swap(s.samples);
        std::swap(zones, s.zones);
        labels = s.labels;
        from = s.window_from;
        s.window_from = wall::now();
        dropped = s.dropped;
        s.dropped = 0;
    }
    const i64 from_s = wall::to_time_t(from);
    const i64 until_s = std::max<i64>(wall::to_time_t(wall::now()), from_s + 1);

    FoldedProfile stacks;
    std::vector<std::string> names;
    for (const auto& [key, count] : samples) {
        const auto* frames = reinterpret_cast<const uintptr_t*>(key.data());
        const size_t n = key.size() / sizeof(uintptr_t);
        names.clear();
        for (size_t i = 0; i < n; ++i) names.push_back(symbol_for(s, frames[i], i > 0));
        // backtrace()'s view starts in the handler itself
        for (size_t i = 0; i < names.size(); ++i) {
            if (names[i].find("on_sigprof") != std::string::npos) {
                names.erase(names.begin(), names.begin() + static_cast<std::ptrdiff_t>(
                                                               std::min(i + 2, names.size())));
                break;
            }
        }
        // The lr the handler kept when it was the sampled function's own
        if (names.size() > 1 && names[1] == names[0]) names.erase(names.begin() + 1);
        std::string stack;
        for (auto it = names.rbegin(); it != names.rend(); ++it) {
            if (!stack.empty()) stack += ';';
            stack += *it;
        }
        stacks.add(std::move(stack), count);
    }
    if (dropped > 0) log(spdlog::level::debug, "Pyroscope: {} samples missed their 20 ms", dropped);
    post(s, s.cfg.app + ".wall", labels, from_s, until_s, s.cfg.sample_hz, stacks);
    // A second service, since Pyroscope admits only its own profile types
    post(s, s.cfg.app + "-zones.wall", labels, from_s, until_s, 1000000, zones);
}

void pusher_main(State& s) {
#ifdef __APPLE__
    pthread_setname_np("pyroscope pusher");
#endif
    std::unique_lock lock(s.mu);
    while (s.running.load()) {
        s.cv.wait_for(lock, std::chrono::seconds(s.cfg.window_s),
                      [&] { return !s.running.load(); });
        if (!s.running.load()) break; // stop() flushes once the threads are joined
        lock.unlock();
        flush(s);
        lock.lock();
    }
}

} // namespace

bool start(const Config& config) {
    if (g_state) return true;
    HttpTarget target;
    if (!parse_http_url(config.url, target)) {
        spdlog::error("--pyroscope {}: give http://host:port[/path] (TLS and credentials belong to "
                      "a relay such as Grafana Alloy, see tools/pyroscope)",
                      config.url);
        return false;
    }
    auto* s = new State;
    s->cfg = config;
    s->target = target;
    s->labels = config.labels;
    s->main_thread = pthread_self();
    s->window_from = wall::now();
#if OSC_PYROSCOPE_FP_WALK
    g_stack_hi = reinterpret_cast<uintptr_t>(pthread_get_stackaddr_np(s->main_thread));
    g_stack_lo = g_stack_hi - pthread_get_stacksize_np(s->main_thread);
#endif
    struct sigaction action{};
    action.sa_sigaction = on_sigprof;
    action.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGPROF, &action, nullptr) != 0) {
        spdlog::error("--pyroscope: cannot install the SIGPROF handler: {}", std::strerror(errno));
        delete s;
        return false;
    }
    s->running = true;
    s->sampler = std::thread(sampler_main, std::ref(*s));
    s->pusher = std::thread(pusher_main, std::ref(*s));
    g_state = s;
    spdlog::info("Pyroscope: sampling the main thread at {} Hz, posting every {} s to {}",
                 config.sample_hz, config.window_s, config.url);
    return true;
}

bool active() {
    return g_state != nullptr;
}

void set_label(const std::string& key, const std::string& value) {
    if (!g_state) return;
    std::lock_guard lock(g_state->mu);
    g_state->labels[key] = value;
}

void stop() {
    State* s = g_state;
    if (!s) return;
    s->running = false;
    s->cv.notify_all();
    s->sampler.join();
    s->pusher.join();
    flush(*s);
    signal(SIGPROF, SIG_IGN);
    g_state = nullptr;
    delete s;
}

void record_zone(const char* const* names, u32 depth, f64 self_us) {
    State* s = g_state;
    if (!s || depth == 0) return;
    std::string path;
    for (u32 i = 0; i < depth; ++i) {
        if (i > 0) path += ';';
        path += names[i];
    }
    std::lock_guard lock(s->mu);
    s->zones.add(std::move(path), static_cast<u64>(std::max(self_us, 0.0) + 0.5));
}

} // namespace osc::pyroscope
