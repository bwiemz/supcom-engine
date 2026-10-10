#include "sim/entity.hpp"
#include "sim/collision.hpp"
#include "sim/entity_registry.hpp"

#include <cmath>

namespace osc::sim {

void Entity::set_position(const Vector3& p) {
    position_ = p;
    if (registry_) registry_->notify_position_changed(*this);
}

void Entity::set_collision_shape(const CollisionShape& s) {
    collision_shape_ = s;
    shape_reach_ = collision_reach(s);
    if (registry_) registry_->notify_collision_shape_changed(*this);
}

void TextureScroller::set(const ScrollerSpec& s, Scroll& start, Scroll& end) {
    spec = s;
    if (spec.type == ScrollType::None) {
        end = start;
    } else if (spec.type == ScrollType::PingPong) {
        in_ping[0] = false;
        in_ping[1] = false;
        countdown[0] = 0;
        countdown[1] = 0;
    }
}

namespace {

Vector3 side_axis(const Quaternion& q) {
    return {1.0f - 2.0f * (q.y * q.y + q.z * q.z), 2.0f * (q.x * q.y + q.w * q.z),
            2.0f * (q.x * q.z - q.w * q.y)};
}

Vector3 forward_axis(const Quaternion& q) {
    return {2.0f * (q.x * q.z + q.w * q.y), 2.0f * (q.y * q.z - q.w * q.x),
            1.0f - 2.0f * (q.x * q.x + q.y * q.y)};
}

f32 forward_move(const Vector3& forward, const Vector3& now, const Vector3& before) {
    return forward.x * (now.x - before.x) + forward.y * (now.y - before.y) +
           forward.z * (now.z - before.z);
}

} // namespace

void TextureScroller::tick(const Vector3& position, const Quaternion& orientation, Scroll& start,
                           Scroll& end) {
    const Vector3 last = last_position;
    const Quaternion last_q = last_orientation;
    last_position = position;
    last_orientation = orientation;
    switch (spec.type) {
    case ScrollType::PingPong: {
        bool changed = false;
        for (int lane = 0; lane < 2; ++lane) {
            if (--countdown[lane] > 0) {
                continue;
            }
            changed = true;
            in_ping[lane] = !in_ping[lane];
            const f32 seconds = in_ping[lane] ? spec.ping_seconds[lane] : spec.pong_seconds[lane];
            countdown[lane] = static_cast<i32>(std::floor(seconds * 10.0f));
        }
        if (changed) {
            start = {in_ping[0] ? spec.ping.u : spec.pong.u,
                     in_ping[1] ? spec.ping.v : spec.pong.v};
            end = start;
        }
        return;
    }
    case ScrollType::Manual:
        start = end;
        end.u += spec.rate.u;
        end.v += spec.rate.v;
        return;
    case ScrollType::MotionDerived: {
        if (position.x == last.x && position.y == last.y && position.z == last.z) {
            return;
        }
        const Vector3 side = side_axis(orientation);
        const Vector3 last_side = side_axis(last_q);
        const Vector3 f = forward_axis(orientation);
        const Vector3 last_f = forward_axis(last_q);
        const Vector3 forward{(f.x + last_f.x) * 0.5f, (f.y + last_f.y) * 0.5f,
                              (f.z + last_f.z) * 0.5f};
        const f32 d = spec.side_dist;
        const f32 lead = forward_move(
            forward, {position.x + side.x * d, position.y + side.y * d, position.z + side.z * d},
            {last.x + last_side.x * d, last.y + last_side.y * d, last.z + last_side.z * d});
        const f32 trail = forward_move(
            forward, {position.x - side.x * d, position.y - side.y * d, position.z - side.z * d},
            {last.x - last_side.x * d, last.y - last_side.y * d, last.z - last_side.z * d});
        start = end;
        end.u += lead * spec.scroll_mult;
        end.v += trail * spec.scroll_mult;
        return;
    }
    case ScrollType::None: return;
    }
}

void Entity::change_scroller(const ScrollerSpec& spec) {
    if (!scroller_) {
        scroller_.emplace();
        scroller_->last_position = position_;
        scroller_->last_orientation = orientation_;
    }
    scroller_->set(spec, scroll_start_, scroll_end_);
}

} // namespace osc::sim
