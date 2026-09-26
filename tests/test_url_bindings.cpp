// OpenURL, as Moho's user state has it (faf-re cfunc_OpenURLL): a URL goes
// to the browser only when it parses and its scheme is one the init file's
// `protocols` allows. FAF's userInit.lua wraps it, and its lobby and score
// screens link out through it.

#include <catch2/catch_test_macros.hpp>

#include "lua/init_loader.hpp"
#include "lua/lua_state.hpp"
#include "lua/url_bindings.hpp"
#include "vfs/virtual_file_system.hpp"

#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#ifndef _WIN32
#include "platform/browser.hpp"

#include <chrono>
#include <cstdlib>
#include <sstream>
#include <thread>
#endif

using osc::lua::url_scheme;

namespace {

struct UrlWorld {
    osc::lua::LuaState state;
    std::vector<std::string> opened;
    osc::lua::UrlOpener opener{{"http", "https", "mailto"},
                               [this](const std::string& url) { opened.push_back(url); }};
    UrlWorld() { osc::lua::register_url_bindings(state, &opener); }

    /// Run `code`; the error message, or "" when it ran.
    std::string run(const std::string& code) {
        auto r = state.do_string(code);
        return r.ok() ? std::string() : r.error().message;
    }
};

} // namespace

TEST_CASE("url_scheme reads a URL's scheme", "[url]") {
    CHECK(url_scheme("http://example.com") == "http");
    CHECK(url_scheme("HTTPS://example.com/a?b=c") == "HTTPS");
    CHECK(url_scheme("mailto:someone@example.com") == "mailto");
    CHECK(url_scheme("a+b.c-d:rest") == "a+b.c-d");
    CHECK(url_scheme("file:///etc/passwd") == "file");
    // No scheme, or not a valid one.
    CHECK(url_scheme("example.com").empty());
    CHECK(url_scheme(":nothing").empty());
    CHECK(url_scheme("1http://example.com").empty());
    CHECK(url_scheme("ht_tp://example.com").empty());
    // Whitespace and control characters: no URL at all.
    CHECK(url_scheme("http://example.com/a b").empty());
    CHECK(url_scheme("http://example.com/\n").empty());
    CHECK(url_scheme(" http://example.com").empty());
}

TEST_CASE("OpenURL opens only the protocols the init file allows", "[url]") {
    UrlWorld w;
    CHECK(w.run("OpenURL('https://github.com/FAForever/fa/releases')").empty());
    CHECK(w.run("OpenURL('HTTP://example.com')").empty()); // any case
    CHECK(w.run("OpenURL('mailto:someone@example.com')").empty());
    CHECK(w.opened == std::vector<std::string>{"https://github.com/FAForever/fa/releases",
                                               "HTTP://example.com", "mailto:someone@example.com"});
    w.opened.clear();
    CHECK(w.run("OpenURL('file:///etc/passwd')").empty());
    CHECK(w.run("OpenURL('javascript:alert(1)')").empty());
    CHECK(w.run("OpenURL('example.com')").empty());
    CHECK(w.run("OpenURL('https://example.com/a b')").empty());
    CHECK(w.run("OpenURL(42)").empty()); // a number reads as the string "42"
    CHECK(w.opened.empty());
}

TEST_CASE("OpenURL takes one string", "[url]") {
    UrlWorld w;
    CHECK(w.run("OpenURL()").find("expected 1 args, but got 0") != std::string::npos);
    CHECK(w.run("OpenURL('http://a', 'http://b')").find("expected 1 args, but got 2") !=
          std::string::npos);
    CHECK(w.run("OpenURL({})").find("string expected") != std::string::npos);
    CHECK(w.opened.empty());
}

TEST_CASE("OpenURL with no allowed protocols opens nothing", "[url]") {
    UrlWorld w;
    w.opener.protocols.clear();
    CHECK(w.run("OpenURL('https://example.com')").empty());
    CHECK(w.opened.empty());
}

TEST_CASE("init loader records the init script's protocols", "[url]") {
    namespace fs = std::filesystem;
    std::random_device rd;
    const fs::path dir =
        fs::temp_directory_path() / ("osc_url_init_" + std::to_string(rd()) + std::to_string(rd()));
    fs::create_directories(dir / "bin");
    const fs::path init = dir / "bin" / "init.lua";
    std::ofstream(init) << "path = {}\nprotocols = { 'http', 'https', 'mailto', 7 }\n";

    osc::lua::LuaState state;
    osc::vfs::VirtualFileSystem vfs;
    osc::lua::InitConfig config;
    config.init_file = init;
    config.fa_path = dir;
    osc::lua::InitLoader loader;
    const auto result = loader.execute_init(state, config, vfs);
    std::error_code ec;
    fs::remove_all(dir, ec);

    REQUIRE(result.ok());
    // Its strings, in order (a number is no protocol name).
    CHECK(loader.url_protocols() == std::vector<std::string>{"http", "https", "mailto"});
}

#ifndef _WIN32
TEST_CASE("open_url hands the URL to the handler as one argument, with no shell", "[url]") {
    namespace fs = std::filesystem;
    std::random_device rd;
    const fs::path dir =
        fs::temp_directory_path() / ("osc_url_open_" + std::to_string(rd()) + std::to_string(rd()));
    fs::create_directories(dir);
    const fs::path got = dir / "got.txt";
    // A stand-in handler on PATH (xdg-open; open on macOS) that records its
    // arguments, one per line, then signals it is done.
    for (const char* name : {"xdg-open", "open"}) {
        const fs::path script = dir / name;
        std::ofstream(script) << "#!/bin/sh\nfor a in \"$@\"; do printf '%s\\n' \"$a\"; done > '"
                              << got.string() << ".tmp'\nmv '" << got.string() << ".tmp' '"
                              << got.string() << "'\n";
        fs::permissions(script, fs::perms::owner_all);
        REQUIRE(fs::is_regular_file(script));
    }
    const char* old_path = std::getenv("PATH");
    // PATH as it was, however the test ends.
    struct RestorePath {
        std::string saved;
        ~RestorePath() { setenv("PATH", saved.c_str(), 1); }
    } restore{old_path ? old_path : ""};
    // The stand-in must come first on PATH, or the real handler would open
    // a browser.
    const std::string path = dir.string() + ":" + restore.saved;
    REQUIRE(setenv("PATH", path.c_str(), 1) == 0);
    const char* now = std::getenv("PATH");
    REQUIRE(now);
    REQUIRE(path == now);

    // Shell syntax in the URL stays in the URL.
    const std::string url = "https://example.com/?a=1;touch${IFS}pwned&b=$(id)`id`";
    REQUIRE(osc::platform::open_url(url));

    for (int i = 0; i < 200 && !fs::exists(got); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    std::ifstream in(got);
    std::stringstream args;
    args << in.rdbuf();
    const bool pwned = fs::exists(dir / "pwned") || fs::exists("pwned");
    std::error_code ec;
    fs::remove_all(dir, ec);
    CHECK(args.str() == url + "\n");
    CHECK_FALSE(pwned);
}
#endif
