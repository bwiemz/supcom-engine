#include "renderer/runtime_decals.hpp"

#include "sim/decal.hpp"
#include "sim/world_snapshot.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace osc::renderer {

std::string resolve_decal_texture(const std::string& name, bool splat) {
    if (name.empty()) return {};
    // A path (Moho also takes a drive's or UNC's, which VFS paths never are).
    if (name.front() == '/' || name.front() == '\\') return name;
    return std::string(splat ? "/env/common/splats/" : "/env/common/decals/") + name + ".dds";
}

std::optional<map::DecalType> decal_type_named(std::string_view name) {
    // CWldTerrainDecal::sTypeDesc, by type number.
    static constexpr std::array<std::string_view, 10> kNames = {
        "Undefined",     "Albedo", "Normals",       "Water Mask", "Water Albedo",
        "Water Normals", "Glow",   "Alpha Normals", "Glow Mask",  "AlbedoXP"};
    for (size_t i = 1; i < kNames.size(); ++i)
        if (kNames[i] == name) return static_cast<map::DecalType>(i);
    if (name != kNames[0]) spdlog::warn("unknown decal type: {}", name);
    return std::nullopt;
}

void RuntimeDecals::clear() {
    decals_.clear();
    splats_.clear();
    taken_.clear();
    last_tick_.reset();
    last_focus_ = -1;
    ++generation_;
}

void RuntimeDecals::update(const sim::WorldSnapshot& snap, i32 focus_army) {
    if (last_tick_ && snap.tick < *last_tick_) clear(); // a new game
    const bool new_tick = !last_tick_ || snap.tick != *last_tick_;
    if (!new_tick && focus_army == last_focus_) return;

    // The beats since the last this saw fade first, as each beat faded
    // them; what changed is only known now, at this tick's beat.
    if (new_tick && last_tick_) {
        // Every fade ends within 34 beats: a longer gap needs no more.
        const u32 from = std::max(*last_tick_ + 1, snap.tick > 64 ? snap.tick - 64 : 0u);
        for (u32 t = from; t < snap.tick; ++t) fade(t);
    }

    // AddDecals: what the focus army sees and this hasn't taken, in the
    // order they were made.
    seen_.clear();
    for (size_t i = 0; i < snap.effects.size(); ++i) {
        const sim::EffectRecord& fx = snap.effects[i];
        if (!fx.decal) continue;
        const bool sees = focus_army < 0 ||
                          (focus_army < 32 && ((fx.seen_by >> static_cast<u32>(focus_army)) & 1u));
        if (!sees) continue;
        seen_.push_back(fx.id);
        if (!std::binary_search(taken_.begin(), taken_.end(), fx.id)) add(snap, i);
    }
    // Made in ascending id, as they come; sorted all the same.
    if (!std::is_sorted(seen_.begin(), seen_.end())) std::sort(seen_.begin(), seen_.end());
    const auto sees = [&](u32 id) { return std::binary_search(seen_.begin(), seen_.end(), id); };
    // RemoveDecals: those it no longer sees, gone from the sim or not.
    for (auto* list : {&decals_, &splats_})
        for (Decal& d : *list)
            if (d.live && !sees(d.id)) {
                d.live = false;
                d.remove_tick = 1;
            }
    // Taken, and still seen: what it sees (each one taken, or added now).
    taken_.swap(seen_);

    // ProcessRemovals, this beat's.
    if (new_tick) fade(snap.tick);
    last_tick_ = snap.tick;
    last_focus_ = focus_army;
}

void RuntimeDecals::add(const sim::WorldSnapshot& snap, size_t effect) {
    const sim::EffectRecord& fx = snap.effects[effect];
    const sim::DecalSpec& spec = *fx.decal;
    Decal d;
    d.id = fx.id;
    d.splat = spec.splat;
    d.remove_tick = spec.remove_tick;
    d.fidelity = spec.fidelity;
    if (spec.splat) {
        d.info.type = map::DecalType::Albedo;
    } else {
        const std::optional<map::DecalType> type = decal_type_named(spec.type);
        if (!type) return;
        d.info.type = *type;
        d.info.texture2_path = resolve_decal_texture(spec.texture2, false);
    }
    d.info.texture_path = resolve_decal_texture(spec.texture1, spec.splat);
    d.info.position_x = spec.position.x;
    d.info.position_y = spec.position.y;
    d.info.position_z = spec.position.z;
    d.info.scale_x = spec.size_x;
    d.info.scale_y = 1.0f;
    d.info.scale_z = spec.size_z;
    d.info.rotation_y = spec.rotation_y;
    // Its own cutoff, or one from its size (ComputeCutoffLOD: the bounds'
    // diagonal times ren_DecalAlbedoLodCutoff or ren_DecalNormalLodCutoff,
    // both 1).
    const sim::DecalBounds b = sim::decal_bounds(spec);
    d.info.cut_off_lod =
        spec.lod > 0.0f ? spec.lod : std::hypot(b.max_x - b.min_x, b.max_z - b.min_z);
    d.info.near_cut_off_lod = 0.0f;
    if (spec.splat) {
        splats_.push_back(std::move(d));
    } else {
        decals_.push_back(std::move(d));
        ++generation_;
    }
}

void RuntimeDecals::fade(u32 tick) {
    // MoveAlphaTowardZero, once the tick passes a decal's removal tick; at
    // 0 it is gone.
    const auto step = [&](std::vector<Decal>& list, f32 by) {
        return std::erase_if(list, [&](Decal& d) {
            if (d.remove_tick == 0 || tick <= d.remove_tick) return false;
            d.alpha = std::max(d.alpha - by, 0.0f);
            return d.alpha == 0.0f;
        });
    };
    if (step(decals_, kDecalFade) > 0) ++generation_;
    (void)step(splats_, kSplatFade);
}

} // namespace osc::renderer
