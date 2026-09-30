#pragma once

// The world's loops as the audio side hears them (M216b): each entity's
// wanted loops (Entity::ambient_sounds), where the frame draws it and
// whether the world camera sees it, for SoundManager::sync_entity_loops.

#include "audio/sound_manager.hpp"
#include "core/types.hpp"

#include <functional>
#include <string_view>
#include <vector>

namespace osc::sim {
class Entity;
class SimState;
struct Vector3;
} // namespace osc::sim

namespace osc::app {

/// The key of entity `entity_id`'s loop slot `slot`.
u64 entity_loop_key(u32 entity_id, std::string_view slot);

/// Every live entity's wanted loops, at `where` it is, in view if `in_view`
/// says so for its position and radius; underwater for a sub or a seabed
/// unit. In entity id order.
std::vector<audio::SoundManager::EntityLoop>
gather_entity_loops(const sim::SimState& sim,
                    const std::function<sim::Vector3(const sim::Entity&)>& where,
                    const std::function<bool(const sim::Vector3&, f32 radius)>& in_view);

} // namespace osc::app
