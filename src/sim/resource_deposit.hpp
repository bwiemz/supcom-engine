#pragma once

#include "core/types.hpp"

namespace osc::sim {

/// Resource deposit (mass/hydrocarbon point on map).
struct ResourceDeposit {
    f32 x = 0, y = 0, z = 0;
    f32 size = 1.0f;
    enum Type : u8 { Mass = 0, Hydrocarbon = 1 } type = Mass;
};

/// A deposit's square on the map grid, as Moho keeps it (faf-re
/// CSimResources BuildRectFromPointAndSize): from its centre less half its
/// whole size, truncated, to that plus the size.
struct DepositRect {
    i32 x0 = 0, z0 = 0, x1 = 0, z1 = 0;
};
inline DepositRect deposit_rect(const ResourceDeposit& d) {
    const i32 size = static_cast<i32>(d.size);
    const i32 x0 = static_cast<i32>(d.x - static_cast<f32>(size) * 0.5f);
    const i32 z0 = static_cast<i32>(d.z - static_cast<f32>(size) * 0.5f);
    return {x0, z0, x0 + size, z0 + size};
}

} // namespace osc::sim
