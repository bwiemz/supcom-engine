#include "attack_move_click_test.hpp"

#include "app/app.hpp"
#include "app/app_internal.hpp"
#include "lua/lua_state.hpp"
#include "map/terrain.hpp"
#include "render_probe.hpp"
#include "renderer/input_handler.hpp"
#include "renderer/renderer.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace osc::test {

namespace {

constexpr std::size_t kClicks = 5;
constexpr u32 kFirstClickTick = 100;
constexpr std::array<std::array<f32, 2>, kClicks> kPoints{
    {{686, 338}, {658, 338}, {686, 354}, {658, 354}, {686, 338}}};

constexpr const char* kAttackMove = R"(
    import('/lua/ui/game/commandmode.lua').StartCommandMode('order', {
        name = 'RULEUCC_Script', AbilityName = 'AttackMove', TaskName = 'AttackMove',
        Cursor = 'ATTACK_MOVE' })
)";

bool sim_lua(app::Engine& e, const char* code) {
    auto r = e.sim_lua_state->do_string(code);
    if (!r) {
        spdlog::error("attack-move-click-test sim Lua: {}", r.error().message);
    }
    return static_cast<bool>(r);
}

} // namespace

void AttackMoveClickProbe::on_frame(app::Engine& e, app::Frame& frame) {
    sim::SimState& sim = *e.sim_state;
    const u32 tick = sim.tick_count();
    const bool ticked = tick != last_tick_;
    last_tick_ = tick;
    const auto screen = [&] {
        const auto w = static_cast<f32>(frame.renderer.ui_width());
        const auto h = static_cast<f32>(frame.renderer.ui_height());
        const sim::Vector3 at{x_, sim.terrain()->get_surface_height(x_, z_), z_};
        return renderer::screen_point(frame.renderer.camera().view_proj(h > 0 ? w / h : 1.0f), at,
                                      w, h);
    };
    switch (step_) {
    case Step::Spawn:
        if (tick >= 1 && sim_lua(e, R"(
                for i = 0, 2 do
                    local x, z = 662 + 3 * i, 348
                    CreateUnitHPR('uel0201', 'ARMY_1', x, GetSurfaceHeight(x, z), z, 0, 0, 0)
                end
            )")) {
            step_ = Step::Select;
        }
        break;
    case Step::Select:
        if (tick < kFirstClickTick) {
            break;
        }
        units_.clear();
        sim.entity_registry().for_each_unit([&](const sim::Entity& u) {
            if (!u.destroyed() && u.army() == 0 && u.blueprint_id() == "uel0201") {
                units_.push_back(u.entity_id());
            }
        });
        if (units_.size() == 3) {
            frame.input.set_selected({units_.begin(), units_.end()});
            step_ = Step::WaitTick;
        }
        break;
    case Step::WaitTick:
        if (!ticked) {
            break;
        }
        wait_ = static_cast<int>(clicks_.size());
        step_ = Step::Press;
        [[fallthrough]];
    case Step::Press:
        if (wait_ > 0) {
            --wait_;
            break;
        }
        x_ = kPoints[clicks_.size()][0];
        z_ = kPoints[clicks_.size()][1];
        at_ = screen();
        if (!at_ || app::mouse_over_ui(e.ui_lua_state.raw(), (*at_)[0], (*at_)[1])) {
            covered_ = true;
            step_ = Step::Done;
            break;
        }
        clicks_.push_back({static_cast<int>(clicks_.size()) + 1, false, false, {}});
        if (!e.ui_lua_state.do_string(kAttackMove)) {
            clicks_.back().queues = "no attack-move mode";
            step_ = Step::Done;
            break;
        }
        frame.renderer.set_scripted_pointer({(*at_)[0], (*at_)[1], 1, 0});
        step_ = Step::Release;
        break;
    case Step::Release:
        frame.renderer.set_scripted_pointer({(*at_)[0], (*at_)[1], 0, 0});
        released_at_ = tick;
        after_release_ = 0;
        step_ = Step::Settle;
        break;
    case Step::Settle: {
        if (++after_release_ == 1) {
            clicks_.back().ticked_before_send = ticked;
        }
        if (tick < released_at_ + 6) {
            break;
        }
        Click& click = clicks_.back();
        click.moved = true;
        for (const u32 id : units_) {
            const auto* u = static_cast<const sim::Unit*>(sim.entity_registry().find(id));
            const auto& queue = u->command_queue();
            const bool moving = !queue.empty() &&
                                queue.back().type == sim::CommandType::AggressiveMove &&
                                std::abs(queue.back().target_pos.x - x_) < 0.5f &&
                                std::abs(queue.back().target_pos.z - z_) < 0.5f;
            click.moved = click.moved && moving;
            click.queues += fmt::format("{}:", id);
            for (const auto& c : queue) {
                click.queues += fmt::format(" {}({:.0f},{:.0f})", static_cast<int>(c.type),
                                            c.target_pos.x, c.target_pos.z);
            }
            click.queues += "; ";
        }
        step_ = clicks_.size() < kClicks ? Step::WaitTick : Step::Done;
        break;
    }
    case Step::Done: break;
    }
}

void AttackMoveClickProbe::finish() const {
    Tally t;
    for (const Click& c : clicks_) {
        t.check(c.moved, fmt::format("released {} frame(s) after a tick{}: {}", c.frames_after_tick,
                                     c.ticked_before_send ? ", the next frame ticking first" : "",
                                     c.moved ? "an AggressiveMove to the click" : c.queues));
    }
    t.check(!covered_, "the clicks' points are the world's, not the UI's");
    t.check(clicks_.size() == kClicks,
            fmt::format("{} of {} clicks made", clicks_.size(), kClicks));
    t.check(std::any_of(clicks_.begin(), clicks_.end(),
                        [](const Click& c) { return c.ticked_before_send; }),
            "a click's next frame ticked first");
}

} // namespace osc::test
