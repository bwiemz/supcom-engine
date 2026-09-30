#include "app/world_sounds.hpp"

#include "sim/entity.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"

#include <algorithm>

namespace osc::app {

u64 entity_loop_key(u32 entity_id, std::string_view slot) {
    u32 hash = 2166136261u; // FNV-1a
    for (const char c : slot) hash = (hash ^ static_cast<unsigned char>(c)) * 16777619u;
    return (static_cast<u64>(entity_id) << 32) | hash;
}

std::vector<audio::SoundManager::EntityLoop>
gather_entity_loops(const sim::SimState& sim,
                    const std::function<sim::Vector3(const sim::Entity&)>& where,
                    const std::function<bool(const sim::Vector3&, f32 radius)>& in_view) {
    std::vector<audio::SoundManager::EntityLoop> loops;
    sim.entity_registry().for_each([&](const sim::Entity& e) {
        if (e.destroyed() || e.ambient_sounds().empty()) return;
        const sim::Vector3 at = where(e);
        bool underwater = false;
        if (e.is_unit()) {
            const std::string& layer = static_cast<const sim::Unit&>(e).layer();
            underwater = layer == "Sub" || layer == "Seabed";
        }
        const f32 radius = std::max({1.0f, e.footprint_size_x(), e.footprint_size_z()}) * 0.5f;
        const bool seen = in_view(at, radius);
        for (const auto& a : e.ambient_sounds())
            loops.push_back({entity_loop_key(e.entity_id(), a.name), a.bank, a.cue, a.lod_cutoff,
                             at, underwater, seen});
    });
    return loops;
}

} // namespace osc::app
