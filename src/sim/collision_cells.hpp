#pragma once

#include "core/types.hpp"

#include <optional>
#include <vector>

namespace osc::sim {

class Entity;

/// Moho's collision-cell database (EntityOccupationManager), for units and
/// props. The map is cut into cells 4 units square, and each cell lists the
/// units and the props whose collision box (its world bounds) touches it,
/// the last linked first. GetUnitsInRect and GetReclaimablesInRect gather
/// from it: they take whatever touches the rectangle's cells, not what
/// stands in it, and give it in the cells' order.
///
/// An entity links when it is made and when its collision shape changes.
/// One that moved or turned relinks in advance(), at the end of the tick
/// (Moho's AdvanceCoords), and only if its cells changed. One without a
/// shape is in no cell. An entity leaves when it is freed: Moho deletes a
/// destroyed one at the end of the beat, so until then a gather finds it.
class CollisionCells {
public:
    /// A block of cells (Moho's CollisionDBRect).
    struct Rect {
        u16 x = 0, z = 0, w = 0, h = 0;
    };

    /// Size the cells for a map `width` by `height` units and empty them.
    /// Moho sizes them by its heightfield, less one vertex: the map's size.
    void init(u32 width, u32 height);
    bool initialized() const { return width_ > 0 && height_ > 0; }
    /// Empty every cell, keeping their size (a snapshot's load refills them).
    void clear();

    /// The cells a query rectangle covers (func_Rect2fToInt16): its corners
    /// truncated to whole units, the far ones rounded up to a cell, at least
    /// one cell each way.
    static Rect query_rect(f32 x0, f32 z0, f32 x1, f32 z1);
    /// The cells a world box covers (func_AABoxToRect): its corners floored
    /// and ceiled to whole units, the far ones rounded up to a cell.
    static Rect box_rect(f32 min_x, f32 min_z, f32 max_x, f32 max_z);

    /// Link `e` where its collision box is now, relinking it if its cells
    /// changed, or unlink it if it has no shape (Moho's UpdateRect). Only
    /// units and props link.
    void link(Entity& e);
    /// Take `e` out (Moho's CollisionShapeBase::Remove).
    void unlink(Entity& e);
    /// Note that `e` moved or turned: advance() relinks it.
    void moved(Entity& e);
    /// Relink each entity that moved since the last call, in the order of
    /// their last moves.
    void advance();

    /// Each entity in the cells `r` covers, once, in Moho's order: cell by
    /// cell along each row, row after row, and in each cell its units (with
    /// `units`), then its props (with `props`), the last linked first.
    std::vector<Entity*> gather(const Rect& r, bool units, bool props);

    /// How many links have been made: a snapshot keeps it, so that links
    /// after a load go on in the same order.
    u64 link_count() const { return seq_; }
    /// After a load: link each of `entities` with its saved order (its
    /// CellLink's seq, its cells taken from where it is), then go on
    /// counting from `count`.
    void restore(std::vector<Entity*> entities, u64 count);

private:
    i32 width_ = 0;      ///< cells across (Moho's mWidth)
    i32 height_ = 0;     ///< cells down
    i32 shift_ = 0;      ///< log2 of width_, rounded down (Moho's mGridWidthShift)
    u32 last_index_ = 0; ///< the cell index mask (Moho's mLastIndex)
    /// Each cell's units and props, the last linked at the back.
    std::vector<std::vector<Entity*>> units_;
    std::vector<std::vector<Entity*>> props_;
    struct Move {
        Entity* entity;
        u64 stamp;
    };
    std::vector<Move> moves_; ///< since the last advance(), in order
    u64 seq_ = 0;
    u64 move_stamp_ = 0;
    u64 gather_ = 0;

    /// The cells `e`'s collision box covers now, none without a shape.
    std::optional<Rect> cells_of(const Entity& e) const;
    std::vector<std::vector<Entity*>>* lists_for(const Entity& e);
    void add(Entity& e);
    void remove(Entity& e);
};

} // namespace osc::sim
