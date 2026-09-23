#include "sim/world_snapshot.hpp"

#include "sim/sim_state.hpp"
#include "sim/unit.hpp"

#include <algorithm>
#include <cmath>

namespace osc::sim {

namespace {

Vector3 lerp(const Vector3& a, const Vector3& b, f32 t) {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
}

/// Normalized lerp along the shorter arc (q and -q are the same rotation).
Quaternion nlerp(const Quaternion& a, const Quaternion& b, f32 t) {
    const f32 dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    const f32 s = dot < 0.0f ? -1.0f : 1.0f;
    Quaternion r{a.x + (s * b.x - a.x) * t, a.y + (s * b.y - a.y) * t,
                 a.z + (s * b.z - a.z) * t, a.w + (s * b.w - a.w) * t};
    const f32 len = std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z + r.w * r.w);
    if (len > 1e-8f) {
        r.x /= len;
        r.y /= len;
        r.z /= len;
        r.w /= len;
    }
    return r;
}

} // namespace

const EntityPose* WorldSnapshot::find(u32 id) const {
    auto it = std::lower_bound(entities.begin(), entities.end(), id,
                               [](const EntityPose& p, u32 v) { return p.id < v; });
    return it != entities.end() && it->id == id ? &*it : nullptr;
}

void capture_world(const SimState& sim, WorldSnapshot& out) {
    out.tick = sim.tick_count();
    out.entities.clear();
    out.bones.clear();
    const auto& registry = sim.entity_registry();
    registry.for_each([&](const Entity& e) {
        if (e.destroyed()) return;
        EntityPose p;
        p.id = e.entity_id();
        p.position = e.position();
        p.orientation = e.orientation();
        p.beam_end = e.beam_endpoint();
        p.snap_serial = e.snap_serial();
        if (e.is_unit()) {
            const auto& pose = static_cast<const Unit&>(e).animated_bone_matrices();
            if (!pose.empty()) {
                p.bone_offset = static_cast<u32>(out.bones.size());
                p.bone_count = static_cast<u32>(pose.size());
                out.bones.insert(out.bones.end(), pose.begin(), pose.end());
            }
        }
        out.entities.push_back(p);
    });
    // The registry is a hash map; the snapshot is ordered for lookup.
    std::sort(out.entities.begin(), out.entities.end(),
              [](const EntityPose& a, const EntityPose& b) { return a.id < b.id; });
}

void WorldHistory::capture(const SimState& sim) {
    cur_ = 1 - cur_;
    capture_world(sim, snaps_[cur_]);
    ++captured_;
}

void WorldHistory::clear() {
    for (auto& s : snaps_) s.clear();
    captured_ = 0;
}

FrameView::Pair FrameView::lookup(u32 id) const {
    if (!cur_) return {};
    const EntityPose* to = cur_->find(id);
    if (!to) return {};
    const EntityPose* from = prev_ ? prev_->find(id) : nullptr;
    if (from && from->snap_serial != to->snap_serial) from = nullptr;
    return {from, to};
}

Vector3 FrameView::position(const Entity& e) const {
    const Pair p = lookup(e.entity_id());
    if (!p.to) return e.position();
    if (!p.from) return p.to->position;
    return lerp(p.from->position, p.to->position, alpha_);
}

Quaternion FrameView::orientation(const Entity& e) const {
    const Pair p = lookup(e.entity_id());
    if (!p.to) return e.orientation();
    if (!p.from) return p.to->orientation;
    return nlerp(p.from->orientation, p.to->orientation, alpha_);
}

Vector3 FrameView::beam_end(const Entity& e) const {
    const Pair p = lookup(e.entity_id());
    if (!p.to) return e.beam_endpoint();
    if (!p.from) return p.to->beam_end;
    return lerp(p.from->beam_end, p.to->beam_end, alpha_);
}

bool FrameView::bones(u32 id, std::vector<BoneMatrix>& out) const {
    const Pair p = lookup(id);
    if (!p.to || p.to->bone_count == 0) return false;
    const auto to = cur_->bones_of(*p.to);
    out.assign(to.begin(), to.end());
    // A changed bone count is a different skeleton: nothing to blend from.
    if (!p.from || p.from->bone_count != p.to->bone_count) return true;
    const auto from = prev_->bones_of(*p.from);
    for (size_t b = 0; b < out.size(); ++b)
        for (size_t i = 0; i < 16; ++i)
            out[b][i] = from[b][i] + (to[b][i] - from[b][i]) * alpha_;
    return true;
}

} // namespace osc::sim
