#pragma once

// An army's influence map (M207b): Moho's CInfluenceMap, the grid of threat
// its AI queries. It holds what the army's intel has reported, not every
// unit: a unit out of sight fades, a structure stays until the army sees it
// gone. See docs/plans/2026-09-25-m207b-influence-map-design.md.

#include "core/types.hpp"
#include "sim/entity.hpp" // Vector3

#include <array>
#include <functional>
#include <map>
#include <optional>
#include <string_view>
#include <vector>

namespace osc::sim {

/// Moho's EThreatType, in its order; the Lua names drop "THREATTYPE_".
enum class ThreatType : u8 {
    Overall,
    OverallNotAssigned,
    Structures,
    StructuresNotMex,
    Naval,
    Land,
    Air,
    Experimental,
    Commander,
    Artillery,
    AntiAir,
    AntiSurface,
    AntiSub,
    Economy,
    Unknown,
};

/// The type named `name` (case-sensitive, as Moho's enum lookup); false if
/// there is none.
bool threat_type_from_name(std::string_view name, ThreatType& out);

/// What an entry keeps of its unit, which it outlives: the blueprint's
/// Defense.*ThreatLevel values and what kind of unit it is.
struct ThreatSource {
    f32 air = 0;
    f32 surface = 0;
    f32 sub = 0;
    f32 economy = 0;
    bool mobile = false;         ///< Physics.MotionType isn't RULEUMT_None
    bool flies = false;          ///< an air unit (Air.CanFly)
    bool mass_extractor = false; ///< MASSEXTRACTION
    bool experimental = false;   ///< EXPERIMENTAL
    bool commander = false;      ///< COMMAND
};

/// Where a mobile unit's threat counts: Land, or Naval (Water, Seabed, Sub).
enum class ThreatLayer : u8 { None, Land, Naval };

/// Cells in the playable area (Moho clips an "on map" query to it).
struct CellRect {
    i32 x0 = 0, z0 = 0, x1 = 0, z1 = 0; ///< inclusive
};

class InfluenceMap {
public:
    /// A map of `map_width` x `map_height` units, kept for `army_count`
    /// armies: cells of max(32, larger side / 16), as Moho's army makes it.
    InfluenceMap(u32 map_width, u32 map_height, u32 army_count);

    i32 grid_size() const { return grid_; }
    i32 width() const { return width_; }
    i32 height() const { return height_; }
    /// The cell a position falls in, clamped to the grid (VectorToCoords).
    i32 cell_of(const Vector3& pos) const;
    /// A cell's centre, as the queries report it.
    Vector3 cell_centre(i32 x, i32 z) const;
    /// The cells of a playable area given in world units.
    CellRect cells_in(f32 x0, f32 z0, f32 x1, f32 z1) const;

    /// The army's intel reports unit `id` of `source_army` at `pos`
    /// (UpdateBlipPosition): a new or moved entry starts afresh; one in the
    /// same cell is refreshed to full strength for another 10 updates.
    void report(u32 id, i32 source_army, const Vector3& pos, const ThreatSource& source);
    /// Drop unit `id`'s entry (a dead structure the army has seen gone).
    void remove(u32 id);
    bool has_entry(u32 id) const { return entry_cells_.count(id) > 0; }
    size_t entry_count() const { return entry_cells_.size(); }
    /// Unit `id`'s entry strength, or 0 without one.
    f32 strength(u32 id) const;

    /// What the army knows now of an entry's unit, which the update reads for
    /// a unit of an army that is neither its own nor allied and still exists.
    struct UnitState {
        ThreatLayer layer = ThreatLayer::None;
        bool detailed = false; ///< in omni now, or ever in line of sight
    };
    /// Moho's Update, every 30 ticks: entries fade (and those at 0 go), the
    /// script-assigned threat fades, and each army's threat is summed again.
    void update(const std::function<bool(i32 army)>& allied_or_self,
                const std::function<std::optional<UnitState>(u32 id)>& unit_state);

    /// A cell's threat of `type` from `army`'s units, or every army's (-1),
    /// plus what scripts assigned (InfluenceGrid::GetThreat).
    f32 cell_threat(i32 x, i32 z, ThreatType type, i32 army) const;
    /// The cells within `radius` of (x, z), a square, clipped to the grid and
    /// to `on_map` when given (GetThreatRect).
    f32 threat_rect(i32 x, i32 z, i32 radius, const CellRect* on_map, ThreatType type,
                    i32 army) const;
    /// The cells from `a`'s to `b`'s, stepped as Bresenham's line, each
    /// summed alone.
    f32 threat_between(const Vector3& a, const Vector3& b, const CellRect* on_map, ThreatType type,
                       i32 army) const;
    struct CellThreat {
        f32 x = 0, z = 0; ///< the cell's centre
        f32 threat = 0;
    };
    /// Each cell within `radius` of `pos`'s with threat above 0, highest
    /// first.
    std::vector<CellThreat> threats_around(const Vector3& pos, i32 radius, const CellRect* on_map,
                                           ThreatType type, i32 army) const;
    /// The cell of most threat, each summed over its square when `radius` >
    /// 0; ties go to the cell nearer `start`.
    CellThreat highest_threat(i32 radius, const CellRect* on_map, ThreatType type, i32 army,
                              const Vector3& start) const;
    /// A script's threat at `pos` (AssignThreatAtPosition): it fades by
    /// `rate` of itself each update (0.01 when negative).
    void assign_threat(const Vector3& pos, ThreatType type, f32 value, f32 rate);

    /// What the sim reads of an entry.
    struct EntryView {
        u32 id = 0;
        i32 source_army = -1;
        Vector3 position;
        bool mobile = false;
        f32 strength = 0;
    };
    /// Each entry, in unit id order.
    template <typename F> void for_each_entry(F&& fn) const {
        for (const auto& [id, cell] : entry_cells_) {
            const Entry& e = cells_[static_cast<size_t>(cell)].entries.at(id);
            fn(EntryView{id, e.source_army, e.position, e.source.mobile, e.strength});
        }
    }

private:
    /// Moho's SThreat lanes, by meaning.
    enum Lane : u8 {
        kOverall,
        kStructures,
        kStructuresNotMex,
        kNaval,
        kLand,
        kAir,
        kExperimental,
        kCommander,
        kArtillery,
        kAntiAir,
        kAntiSurface,
        kAntiSub,
        kEconomy,
        kUnknown,
        kLaneCount,
    };
    using Lanes = std::array<f32, kLaneCount>;
    static Lane lane_of(ThreatType type);

    struct Entry {
        i32 source_army = -1;
        Vector3 position;
        ThreatSource source;
        ThreatLayer layer = ThreatLayer::None;
        bool detailed = false;
        f32 strength = 1.0f;
        f32 decay = 0.0f; ///< per update once decay_ticks runs out
        i32 decay_ticks = 10;
    };
    struct Cell {
        std::map<u32, Entry> entries; ///< by unit id
        std::vector<Lanes> threats;   ///< per source army, rebuilt each update
        Lanes assigned{};             ///< scripts'
        Lanes assigned_decay{};
    };

    void insert(u32 id, i32 source_army, const Vector3& pos, const ThreatSource& source);
    const Cell* cell_at(i32 x, i32 z) const;

    i32 grid_ = 32;
    i32 width_ = 0;
    i32 height_ = 0;
    std::vector<Cell> cells_;
    std::map<u32, i32> entry_cells_; ///< unit id -> its entry's cell
};

} // namespace osc::sim
