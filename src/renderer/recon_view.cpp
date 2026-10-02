#include "renderer/recon_view.hpp"


#include <algorithm>

namespace osc::renderer {

void ReconView::set_focus_army(i32 army) {
    if (army == focus_) return;
    focus_ = army;
    clear();
}

void ReconView::clear() {
    memory_.clear();
    ghosts_.clear();
    fakes_.clear();
    updated_ = false;
    everything_ = true;
    allies_ = 0;
}

bool ReconView::judged(const sim::EntityRecord& e) const {
    if (!e.is_unit && !e.is_projectile) return false; // shields, beams: by sees_at
    if (e.army < 0 || e.army == focus_) return false;
    return e.army >= 32 || (allies_ >> e.army & 1u) == 0;
}

void ReconView::update(const sim::FrameView& view, std::span<const sim::IntelFlushRecord> flushes) {
    const sim::WorldSnapshot* cur = view.cur();
    if (!cur) return;
    if (updated_ && cur->tick == last_tick_) return;
    updated_ = true;
    last_tick_ = cur->tick;
    ++update_count_;

    // The player's army's sight (none captured for it yet: it sees nothing
    // but its recon of units)
    const sim::SightMap* sight = cur->sight.army == focus_ ? &cur->sight : nullptr;
    everything_ = focus_ < 0 || focus_ >= 32 || (sight && sight->everywhere);
    fakes_.clear();
    if (everything_) {
        memory_.clear();
        ghosts_.clear();
        return;
    }
    // The jammers' fakes it senses: their jammers' records where they are
    // (entity ids stay under 2^26, so the ids can't meet a real one's).
    for (const sim::FakeBlipRecord& f : cur->fake_blips) {
        if (f.viewer != static_cast<u32>(focus_)) continue;
        const sim::EntityRecord* source = cur->find(f.source);
        if (!source) continue;
        sim::EntityRecord& r = fakes_.emplace_back(*source);
        r.id = sim::fake_blip_id(f.source, f.index);
        r.position = f.position;
        r.bone_count = 0; // no pose of its own
        r.is_being_built = false;
    }
    const sim::ArmyRecord* army = cur->army(focus_);
    allies_ = army ? army->allies : 0;

    // What FlushIntelInRect took from the player's army: its blips of these
    // units, and anything it remembers in the rects that is gone.
    std::vector<u32> forgotten;
    for (const auto& f : flushes)
        for (const auto& [id, armies] : f.forgotten)
            if ((armies >> static_cast<u32>(focus_) & 1u) != 0) forgotten.push_back(id);
    std::sort(forgotten.begin(), forgotten.end());
    const auto flushed_at = [&](const sim::Vector3& p) {
        return std::any_of(flushes.begin(), flushes.end(), [&](const sim::IntelFlushRecord& f) {
            return p.x >= static_cast<f32>(f.x0) && p.x <= static_cast<f32>(f.x1) &&
                   p.z >= static_cast<f32>(f.z0) && p.z <= static_cast<f32>(f.z1);
        });
    };

    for (const sim::EntityRecord& e : cur->entities) {
        if (!judged(e)) continue;
        Memory& m = memory_[e.id];
        m.touched = update_count_;
        m.ghost = false; // in the world, whatever the player's army thought

        if (e.is_projectile) {
            // A projectile: the player's line of sight where it is (under
            // the water, its water grid's).
            m.sight = sight && sight->sees(e.position.x, e.position.y, e.position.z)
                          ? Sight::Seen
                          : Sight::Hidden;
            continue;
        }
        if (std::binary_search(forgotten.begin(), forgotten.end(), e.id)) {
            m.seen_ever = false;
            m.pose.clear();
            m.fraction = 1.0f;
            m.last = {};
        }
        // A unit: the sim's recon of it, cloak, stealth and layer counted
        // (M215d).
        const u32 bit = 1u << static_cast<u32>(focus_);
        const bool los = (e.los_now & bit) != 0;
        const bool detected = (e.detected & bit) != 0;
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
            m.last = e; // as it's drawn, should it die out of sight
        } else if (m.seen_ever) {
            m.sight = Sight::Remembered;
        } else {
            m.sight = detected ? Sight::Blip : Sight::Hidden;
        }
    }

    // Entities gone from the world are forgotten, but for a structure the
    // player's army remembers and didn't see go: Moho's MaybeDead, drawn as
    // last seen until its army sees the spot (M215d).
    const auto seen_there = [&](const sim::Vector3& p) {
        return sight && sight->sees(p.x, p.y, p.z);
    };
    ghosts_.clear();
    for (auto it = memory_.begin(); it != memory_.end();) {
        Memory& m = it->second;
        if (m.touched != update_count_) {
            const bool remembered = m.ghost || (m.seen_ever && m.last.id != 0);
            if (!remembered || seen_there(m.last.position) || flushed_at(m.last.position)) {
                it = memory_.erase(it);
                continue;
            }
            m.ghost = true;
            m.sight = Sight::Remembered;
            ghosts_.push_back(m.last);
        }
        ++it;
    }
    // In id order, as the snapshot's entities are.
    std::sort(ghosts_.begin(), ghosts_.end(),
              [](const sim::EntityRecord& a, const sim::EntityRecord& b) { return a.id < b.id; });
}

bool ReconView::maybe_dead(u32 id) const {
    const auto it = memory_.find(id);
    return it != memory_.end() && it->second.ghost;
}

Sight ReconView::sight(const sim::EntityRecord& e) const {
    if ((e.id & kFakeBlip) != 0) return Sight::Blip; // a jammer's fake
    if (everything_ || !judged(e)) return Sight::Seen;
    const auto it = memory_.find(e.id);
    return it == memory_.end() ? Sight::Hidden : it->second.sight;
}

bool ReconView::sees_at(const sim::FrameView& view, i32 army, f32 x, f32 z) const {
    if (everything_ || army == focus_) return true;
    if (army >= 0 && army < 32 && (allies_ >> army & 1u) != 0) return true;
    const sim::WorldSnapshot* cur = view.cur();
    if (!cur) return true;
    if (cur->sight.army != focus_) return false;
    return cur->sight.sees_ground(x, z);
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
