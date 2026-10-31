#pragma once

#include "core/types.hpp"

#include <compare>
#include <functional>

namespace osc::sim {

/// An entity's key: Moho's id (EntityDB's EntId) above, and below it which time its pool
/// handed that id out, so a dead entity's key never finds the next one with its id, as
/// Moho's weak pointers don't. Ordered by key, entities come in Moho's id order. 53 bits,
/// which a Lua number holds exactly.
class EntityId {
public:
    static constexpr u32 kGenerationBits = 21;
    static constexpr u32 kMaxGeneration = (1u << kGenerationBits) - 1;

    constexpr EntityId() = default;
    constexpr explicit EntityId(u64 key) : key_(key) {}
    static constexpr EntityId of(u32 moho, u32 generation) {
        return EntityId((u64{moho} << kGenerationBits) | generation);
    }

    constexpr u64 key() const { return key_; }
    constexpr u32 moho() const { return static_cast<u32>(key_ >> kGenerationBits); }
    constexpr u32 generation() const { return static_cast<u32>(key_) & kMaxGeneration; }
    constexpr explicit operator bool() const { return key_ != 0; }

    friend constexpr bool operator==(EntityId, EntityId) = default;
    friend constexpr auto operator<=>(EntityId, EntityId) = default;

private:
    u64 key_ = 0;
};

/// For fmt and spdlog: the id scripts see.
constexpr u32 format_as(EntityId id) {
    return id.moho();
}

} // namespace osc::sim

template <> struct std::hash<osc::sim::EntityId> {
    size_t operator()(osc::sim::EntityId id) const noexcept {
        return std::hash<osc::u64>{}(id.key());
    }
};
