#include <catch2/catch_test_macros.hpp>

#include "app/app_internal.hpp"
#include "app/scripted_mouse.hpp"

#include <GLFW/glfw3.h>

#include <string>
#include <vector>

using osc::app::MouseStep;
using osc::app::parse_mouse_step;

TEST_CASE("A --mouse step reads its time, its verb and where", "[app][scripted_mouse]") {
    const auto move = parse_mouse_step("12 move 640,360.5");
    REQUIRE(move);
    CHECK(move->clock == MouseStep::Clock::Frame);
    CHECK(move->at == 12);
    CHECK(move->verb == MouseStep::Verb::Move);
    CHECK_FALSE(move->world);
    CHECK(move->x == 640.0);
    CHECK(move->y == 360.5);

    const auto world = parse_mouse_step("t40 move w672,346");
    REQUIRE(world);
    CHECK(world->clock == MouseStep::Clock::Tick);
    CHECK(world->at == 40);
    CHECK(world->world);
    CHECK(world->x == 672.0);
    CHECK(world->y == 346.0);

    const auto press = parse_mouse_step("13 press right");
    REQUIRE(press);
    CHECK(press->verb == MouseStep::Verb::Press);
    CHECK(press->button == GLFW_MOUSE_BUTTON_RIGHT);

    const auto shift = parse_mouse_step("14 release shift");
    REQUIRE(shift);
    CHECK(shift->verb == MouseStep::Verb::Release);
    CHECK(shift->button == -1);
    CHECK(shift->key == GLFW_KEY_LEFT_SHIFT);

    for (const char* bad : {"move 1,2", "x press left", "3 press thumb", "3 move 1", "3 move 1,2,3",
                            "3 jump", "3 press", "3 move w1", "t press left", ""}) {
        INFO(bad);
        CHECK_FALSE(parse_mouse_step(bad));
    }
}

TEST_CASE("The --mouse steps are kept in order, and a bad one stops the run",
          "[app][scripted_mouse]") {
    const auto parse = [](std::vector<std::string> args) {
        args.insert(args.begin(), "opensupcom");
        std::vector<char*> argv;
        argv.reserve(args.size());
        for (auto& a : args) {
            argv.push_back(a.data());
        }
        return osc::app::parse_options(static_cast<int>(argv.size()), argv.data(), {});
    };
    const auto opt = parse({"--mouse", "5 move 10,20", "--golden", "g", "--mouse", "6 press left"});
    REQUIRE(opt);
    REQUIRE(opt->mouse.size() == 2);
    CHECK(opt->mouse[0].verb == MouseStep::Verb::Move);
    CHECK(opt->mouse[1].verb == MouseStep::Verb::Press);
    CHECK(opt->scripted_window);
    CHECK_FALSE(parse({"--mouse", "5 wiggle"}));
}

TEST_CASE("A scripted mouse reaches the window's input at its frames and ticks",
          "[app][scripted_mouse]") {
    std::vector<MouseStep> steps;
    for (const char* text : {"2 move 100,50", "2 press shift", "3 press left", "t7 move w30,40",
                             "t7 press right", "7 release left"}) {
        steps.push_back(*parse_mouse_step(text));
    }
    osc::app::ScriptedMouse mouse(std::move(steps));
    std::vector<std::string> seen;
    osc::app::MouseSink sink;
    sink.cursor = [&](double x, double y) {
        seen.push_back("cursor " + std::to_string(int(x)) + "," + std::to_string(int(y)));
    };
    sink.button = [&](int b, int action, int mods) {
        seen.push_back("button " + std::to_string(b) + " " + std::to_string(action) + " " +
                       std::to_string(mods));
    };
    sink.key = [&](int k, int action, int mods) {
        seen.push_back("key " + std::to_string(k) + " " + std::to_string(action) + " " +
                       std::to_string(mods));
    };
    sink.world_to_screen = [](double x, double z) {
        return std::optional<std::array<double, 2>>{{x * 10, z * 10}};
    };

    mouse.frame(std::nullopt, sink);
    CHECK(seen.empty());
    mouse.frame(std::nullopt, sink);
    CHECK(seen == std::vector<std::string>{"cursor 100,50",
                                           "key " + std::to_string(GLFW_KEY_LEFT_SHIFT) + " 1 1"});
    CHECK(mouse.pointer().x == 100.0);
    CHECK(mouse.pointer().mods == GLFW_MOD_SHIFT);
    seen.clear();
    mouse.frame(std::nullopt, sink);
    CHECK(seen == std::vector<std::string>{"button 0 1 1"});
    CHECK(mouse.pointer().buttons == 1u);
    seen.clear();
    mouse.frame(std::nullopt, sink);
    mouse.frame(6, sink);
    CHECK(seen.empty());
    mouse.frame(7, sink);
    CHECK(seen == std::vector<std::string>{"cursor 300,400", "button 1 1 1"});
    CHECK(mouse.pointer().buttons == 3u);
    CHECK_FALSE(mouse.done());
    CHECK(mouse.never_done() == "--mouse '7 release left': the run ended first");
    seen.clear();
    mouse.frame(8, sink);
    CHECK(seen == std::vector<std::string>{"button 0 0 1"});
    CHECK(mouse.pointer().buttons == 2u);
    CHECK(mouse.done());
}
