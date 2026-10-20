// Pyroscope's folded format and ingest URL (core/pyroscope_folded.hpp): the
// pure part, in every build; the sampling and posting are OSC_PYROSCOPE's.

#include "core/pyroscope_folded.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace osc::pyroscope;

TEST_CASE("pyroscope: the target is plain http, with an optional port and base path",
          "[pyroscope]") {
    HttpTarget t;
    REQUIRE(parse_http_url("http://localhost:4040", t));
    CHECK(t.host == "localhost");
    CHECK(t.port == 4040);
    CHECK(t.base_path.empty());

    REQUIRE(parse_http_url("http://alloy.lan/profiles/", t));
    CHECK(t.host == "alloy.lan");
    CHECK(t.port == 80);
    CHECK(t.base_path == "/profiles");

    CHECK_FALSE(parse_http_url("https://profiles.grafana.net", t));
    CHECK_FALSE(parse_http_url("localhost:4040", t));
    CHECK_FALSE(parse_http_url("http://:4040", t));
    CHECK_FALSE(parse_http_url("http://host:0", t));
    CHECK_FALSE(parse_http_url("http://host:70000", t));
    CHECK_FALSE(parse_http_url("http://host:4o4o", t));
}

TEST_CASE("pyroscope: the ingest query names the app, its labels and the window", "[pyroscope]") {
    HttpTarget t;
    REQUIRE(parse_http_url("http://localhost:4040", t));
    CHECK(ingest_path(t, "opensupcom.wall", {}, 100, 110, 100) ==
          "/ingest?name=opensupcom.wall&from=100&until=110&format=folded&sampleRate=100"
          "&units=samples&spyName=opensupcom");
    // Labels sorted, braces and separators percent-encoded, values sanitized
    const std::map<std::string, std::string> labels{{"mode", "window"}, {"device", "Apple M4 Pro"}};
    CHECK(ingest_path(t, "opensupcom.zones", labels, 1, 2, 1000000) ==
          "/ingest?name=opensupcom.zones%7Bdevice%3DApple_M4_Pro%2Cmode%3Dwindow%7D&from=1&until=2"
          "&format=folded&sampleRate=1000000&units=samples&spyName=opensupcom");
    REQUIRE(parse_http_url("http://relay/base/", t));
    CHECK(ingest_path(t, "a", {}, 1, 2, 100).rfind("/base/ingest?", 0) == 0);
}

TEST_CASE("pyroscope: a folded profile sums repeated stacks, one sorted line each", "[pyroscope]") {
    FoldedProfile p;
    CHECK(p.empty());
    p.add("main;Sim::tick", 1);
    p.add("main;Sim::tick", 2);
    p.add("main", 5);
    CHECK(p.size() == 2);
    CHECK(p.text() == "main 5\nmain;Sim::tick 3\n");
    p.clear();
    CHECK(p.text().empty());
}

TEST_CASE("pyroscope: frame names drop return types and parameters", "[pyroscope]") {
    CHECK(frame_name("osc::sim::SimState::tick()") == "osc::sim::SimState::tick");
    CHECK(frame_name("osc::Renderer::draw(osc::FrameView const&) const") == "osc::Renderer::draw");
    CHECK(frame_name(
              "void std::__1::sort<int*, bool (*)(int, int)>(int*, int*, bool (*)(int, int))") ==
          "std::__1::sort<int*, bool (*)(int, int)>");
    CHECK(frame_name("osc::App::frame()::$_0::operator()() const") == "osc::App::frame");
    CHECK(frame_name("operator()(int)") == "operator()");
    CHECK(frame_name("a;b\nc") == "a,b,c");
    CHECK(frame_name("_ZN3osc3fooEv") == "_ZN3osc3fooEv");
    CHECK(frame_name("") == "");
}
