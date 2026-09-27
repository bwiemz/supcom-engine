#include "renderer/recon_view.hpp"

#include "map/visibility_grid.hpp"

namespace osc::renderer {

void ReconView::set_focus_army(i32 army) {
    if (army == focus_) return;
    focus_ = army;
    clear();
}

void ReconView::clear() {
    memory_.clear();
    updated_ = false;
    everything_ = true;
    allies_ = 0;
}

bool ReconView::judged(const sim::EntityRecord& e) const {
    if (!e.is_unit && !e.is_projectile) return false; // shields, beams: M215b
    if (e.army < 0 || e.army == focus_) return false;
    return e.army >= 32 || (allies_ >> e.army & 1u) == 0;
}

void ReconView::update(const sim::FrameView& view) {
    const sim::WorldSnapshot* cur = view.cur();
    if (!cur) return;
    if (updated_ && cur->tick == last_tick_) return;
    updated_ = true;
    last_tick_ = cur->tick;
    ++update_count_;

    const map::VisibilityGrid* grid = cur->visibility ? &*cur->visibility : nullptr;
    everything_ =
        focus_ < 0 || focus_ >= static_cast<i32>(map::VisibilityGrid::MAX_ARMIES) || !grid;
    if (everything_) {
        memory_.clear();
        return;
    }
    const sim::ArmyRecord* army = cur->army(focus_);
    allies_ = army ? army->allies : 0;

    using map::VisFlag;
    const VisFlag senses = VisFlag::Vision | VisFlag::Radar | VisFlag::Sonar | VisFlag::Omni;
    for (const sim::EntityRecord& e : cur->entities) {
        if (!judged(e)) continue;
        Memory& m = memory_[e.id];
        m.touched = update_count_;

        u32 gx = 0;
        u32 gz = 0;
        grid->world_to_grid(e.position.x, e.position.z, gx, gz);
        const VisFlag here = grid->get(gx, gz, static_cast<u32>(focus_));
        const bool los = map::has_flag(here, VisFlag::Vision);
        const bool detected = map::has_flag(here, senses);

        if (e.is_projectile) {
            m.sight = los ? Sight::Seen : Sight::Hidden;
            continue;
        }
        if (los) m.seen_ever = true;
        if (e.is_mobile) {
            // A mobile unit no sense detects loses its blip, and with it
            // what was seen of it.
            if (!detected) m.seen_ever = false;
            if (los) m.sight = Sight::Seen;
            else if (detected) m.sight = m.seen_ever ? Sight::SeenBlip : Sight::Blip;
            else m.sight = Sight::Hidden;
        } else if (los) {
            m.sight = Sight::Seen;
            const auto bones = cur->bones_of(e);
            m.pose.assign(bones.begin(), bones.end());
            m.fraction = e.fraction_complete;
        } else if (m.seen_ever) {
            m.sight = Sight::Remembered;
        } else {
            m.sight = detected ? Sight::Blip : Sight::Hidden;
        }
    }

    // Entities gone from the world are forgotten (a structure destroyed out
    // of sight is MaybeDead in Moho, and stays until the spot is seen: M215b).
    for (auto it = memory_.begin(); it != memory_.end();)
        it = it->second.touched == update_count_ ? std::next(it) : memory_.erase(it);
}

Sight ReconView::sight(const sim::EntityRecord& e) const {
    if (everything_ || !judged(e)) return Sight::Seen;
    const auto it = memory_.find(e.id);
    return it == memory_.end() ? Sight::Hidden : it->second.sight;
}

bool ReconView::sees_at(const sim::FrameView& view, i32 army, f32 x, f32 z) const {
    if (everything_ || army == focus_) return true;
    if (army >= 0 && army < 32 && (allies_ >> army & 1u) != 0) return true;
    const sim::WorldSnapshot* cur = view.cur();
    if (!cur || !cur->visibility) return true;
    u32 gx = 0;
    u32 gz = 0;
    cur->visibility->world_to_grid(x, z, gx, gz);
    return map::has_flag(cur->visibility->get(gx, gz, static_cast<u32>(focus_)),
                         map::VisFlag::Vision);
}

const std::vector<sim::BoneMatrix>* ReconView::frozen_pose(u32 id) const {
    const auto it = memory_.find(id);
    if (it == memory_.end() || it->second.sight != Sight::Remembered || it->second.pose.empty())
        return nullptr;
    return &it->second.pose;
}

f32 ReconView::frozen_fraction(u32 id, f32 live) const {
    const auto it = memory_.find(id);
    if (it == memory_.end() || it->second.sight != Sight::Remembered) return live;
    return it->second.fraction;
}

} // namespace osc::renderer
