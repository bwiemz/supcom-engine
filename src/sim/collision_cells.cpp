#include "sim/collision_cells.hpp"

#include "sim/collision.hpp"
#include "sim/entity.hpp"

#include <algorithm>
#include <bit>
#include <cmath>

namespace osc::sim {

namespace {

constexpr i32 kCellShift = 2; // cells are 4 units square
constexpr i32 kMaxCell = 0xFFFF;

/// A coordinate fit for converting to an int: within +-2^24 (far off any
/// map), and 0 for NaN. Converting a float out of int's range has no defined
/// result, and peers on other CPUs must agree on what a script's wild rect
/// or a broken box covers.
f32 tame(f32 v) {
    constexpr f32 kLimit = 16777216.0f;
    return std::isnan(v) ? 0.0f : std::clamp(v, -kLimit, kLimit);
}

u16 clamp_start(i32 v) {
    return static_cast<u16>(std::clamp(v, 0, kMaxCell));
}

/// An extent from a clamped start (ClampCollisionCellExtentToU16).
u16 clamp_extent(i32 v, u16 start) {
    return static_cast<u16>(std::clamp(v, 0, kMaxCell - static_cast<i32>(start)));
}

bool same(const CellLink& l, const CollisionCells::Rect& r) {
    return l.x == r.x && l.z == r.z && l.w == r.w && l.h == r.h;
}

} // namespace

void CollisionCells::init(u32 width, u32 height) {
    width_ = static_cast<i32>(width >> kCellShift);
    height_ = static_cast<i32>(height >> kCellShift);
    shift_ = width_ > 0 ? static_cast<i32>(std::bit_width(static_cast<u32>(width_))) - 1 : 0;
    last_index_ = initialized() ? static_cast<u32>(width_ * height_) - 1u : 0u;
    const size_t cells = initialized() ? static_cast<size_t>(width_) * height_ : 0;
    units_.assign(cells, {});
    props_.assign(cells, {});
}

void CollisionCells::clear() {
    for (auto& cell : units_) cell.clear();
    for (auto& cell : props_) cell.clear();
}

CollisionCells::Rect CollisionCells::query_rect(f32 x0, f32 z0, f32 x1, f32 z1) {
    // Truncated toward zero, then shifted (an arithmetic shift, as Moho's).
    const i32 start_x = static_cast<i32>(tame(x0)) >> kCellShift;
    const i32 start_z = static_cast<i32>(tame(z0)) >> kCellShift;
    const i32 end_x = (static_cast<i32>(tame(x1)) + 3) >> kCellShift;
    const i32 end_z = (static_cast<i32>(tame(z1)) + 3) >> kCellShift;
    Rect r;
    r.x = clamp_start(start_x);
    r.z = clamp_start(start_z);
    r.w = static_cast<u16>(std::max(std::min(end_x - r.x, kMaxCell - r.x), 1));
    r.h = static_cast<u16>(std::max(std::min(end_z - r.z, kMaxCell - r.z), 1));
    return r;
}

CollisionCells::Rect CollisionCells::box_rect(f32 min_x, f32 min_z, f32 max_x, f32 max_z) {
    const i32 start_x = static_cast<i32>(std::floor(tame(min_x))) >> kCellShift;
    const i32 start_z = static_cast<i32>(std::floor(tame(min_z))) >> kCellShift;
    const i32 end_x = (static_cast<i32>(std::ceil(tame(max_x))) + 3) >> kCellShift;
    const i32 end_z = (static_cast<i32>(std::ceil(tame(max_z))) + 3) >> kCellShift;
    Rect r;
    r.x = clamp_start(start_x);
    r.z = clamp_start(start_z);
    r.w = clamp_extent(end_x - r.x, r.x);
    r.h = clamp_extent(end_z - r.z, r.z);
    return r;
}

std::optional<CollisionCells::Rect> CollisionCells::cells_of(const Entity& e) const {
    const auto bounds = collision_bounds(e);
    if (!bounds) return std::nullopt;
    Rect r = box_rect(bounds->first.x, bounds->first.z, bounds->second.x, bounds->second.z);
    // No wider or deeper than the map: a box that big covers every cell
    // anyway. Only a broken shape or orientation makes one, and Moho would
    // fill its full span, up to 65535 cells square.
    r.w = static_cast<u16>(std::min<i32>(r.w, width_));
    r.h = static_cast<u16>(std::min<i32>(r.h, height_));
    return r;
}

std::vector<std::vector<Entity*>>* CollisionCells::lists_for(const Entity& e) {
    if (e.is_unit()) return &units_;
    if (e.is_prop()) return &props_;
    return nullptr;
}

void CollisionCells::add(Entity& e) {
    auto* lists = lists_for(e);
    const CellLink& l = e.cell_link();
    if (!lists) return;
    // Rows step by the width; a cell past the map's edge wraps through the
    // mask, as Moho's index does.
    u32 row = static_cast<u32>(l.x) + (static_cast<u32>(l.z) << shift_);
    for (u32 j = 0; j < l.h; ++j, row += static_cast<u32>(width_))
        for (u32 i = 0; i < l.w; ++i) (*lists)[(row + i) & last_index_].push_back(&e);
}

void CollisionCells::remove(Entity& e) {
    auto* lists = lists_for(e);
    const CellLink& l = e.cell_link();
    if (!lists || l.seq == 0) return;
    u32 row = static_cast<u32>(l.x) + (static_cast<u32>(l.z) << shift_);
    for (u32 j = 0; j < l.h; ++j, row += static_cast<u32>(width_)) {
        for (u32 i = 0; i < l.w; ++i) {
            auto& cell = (*lists)[(row + i) & last_index_];
            // The last occurrence: a wrapped box may list it in one cell twice.
            const auto it = std::find(cell.rbegin(), cell.rend(), &e);
            if (it != cell.rend()) cell.erase(std::next(it).base());
        }
    }
}

void CollisionCells::link(Entity& e) {
    if (!initialized() || !lists_for(e)) return;
    CellLink& l = e.cell_link();
    const auto cells = cells_of(e);
    if (!cells) {
        unlink(e);
        return;
    }
    const Rect& r = *cells;
    if (l.seq != 0 && same(l, r)) return; // as RelinkTo, only when its cells change
    remove(e);
    l.x = r.x;
    l.z = r.z;
    l.w = r.w;
    l.h = r.h;
    l.seq = ++seq_;
    add(e);
}

void CollisionCells::unlink(Entity& e) {
    CellLink& l = e.cell_link();
    if (l.seq == 0) return;
    remove(e);
    l = CellLink{};
}

std::vector<Entity*> CollisionCells::gather(const Rect& r, bool units, bool props) {
    std::vector<Entity*> out;
    if (!initialized()) return out;
    const u64 mark = ++gather_;
    const i32 x_count = std::min(static_cast<i32>(r.w), width_ - static_cast<i32>(r.x));
    const i32 z_count = std::min(height_ - static_cast<i32>(r.z), static_cast<i32>(r.h));
    u32 row = static_cast<u32>(r.x) + (static_cast<u32>(r.z) << shift_);
    const auto take = [&](const std::vector<Entity*>& cell) {
        for (auto it = cell.rbegin(); it != cell.rend(); ++it) {
            CellLink& l = (*it)->cell_link();
            if (l.mark == mark) continue;
            l.mark = mark;
            out.push_back(*it);
        }
    };
    for (i32 j = 0; j < z_count; ++j, row += static_cast<u32>(width_)) {
        for (i32 i = 0; i < x_count; ++i) {
            const u32 index = (row + static_cast<u32>(i)) & last_index_;
            if (units) take(units_[index]);
            if (props) take(props_[index]);
        }
    }
    return out;
}

void CollisionCells::restore(std::vector<Entity*> entities, u64 count) {
    std::sort(entities.begin(), entities.end(), [](const Entity* a, const Entity* b) {
        return a->cell_link().seq < b->cell_link().seq;
    });
    for (Entity* e : entities) {
        CellLink& l = e->cell_link();
        const u64 seq = l.seq;
        l = CellLink{};
        if (seq == 0 || !initialized() || !lists_for(*e)) continue;
        const auto cells = cells_of(*e);
        if (!cells) continue;
        l.x = cells->x;
        l.z = cells->z;
        l.w = cells->w;
        l.h = cells->h;
        l.seq = seq;
        add(*e);
    }
    seq_ = count;
}

} // namespace osc::sim
