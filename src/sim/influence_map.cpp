#include "sim/influence_map.hpp"

#include <algorithm>
#include <cstdlib>
#include <limits>

namespace osc::sim {

namespace {

constexpr i32 kFreshTicks = 10;     ///< updates at full strength after a report
constexpr f32 kMobileDecay = 0.02f; ///< per update after that; a structure's is 0

/// Moho's DecayThreatLane: a positive value falls by `decay`, not below 0;
/// 0 or less stays.
f32 decay_lane(f32 value, f32 decay) {
    if (value <= 0.0f) return value;
    return std::max(value - decay, std::min(value + decay, 0.0f));
}

} // namespace

bool threat_type_from_name(std::string_view name, ThreatType& out) {
    static constexpr std::pair<std::string_view, ThreatType> kNames[] = {
        {"Overall", ThreatType::Overall},
        {"OverallNotAssigned", ThreatType::OverallNotAssigned},
        {"Structures", ThreatType::Structures},
        {"StructuresNotMex", ThreatType::StructuresNotMex},
        {"Naval", ThreatType::Naval},
        {"Land", ThreatType::Land},
        {"Air", ThreatType::Air},
        {"Experimental", ThreatType::Experimental},
        {"Commander", ThreatType::Commander},
        {"Artillery", ThreatType::Artillery},
        {"AntiAir", ThreatType::AntiAir},
        {"AntiSurface", ThreatType::AntiSurface},
        {"AntiSub", ThreatType::AntiSub},
        {"Economy", ThreatType::Economy},
        {"Unknown", ThreatType::Unknown},
    };
    for (const auto& [n, t] : kNames) {
        if (n == name) {
            out = t;
            return true;
        }
    }
    return false;
}

InfluenceMap::Lane InfluenceMap::lane_of(ThreatType type) {
    switch (type) {
    case ThreatType::Overall:
    case ThreatType::OverallNotAssigned: return kOverall;
    case ThreatType::Structures: return kStructures;
    case ThreatType::StructuresNotMex: return kStructuresNotMex;
    case ThreatType::Naval: return kNaval;
    case ThreatType::Land: return kLand;
    case ThreatType::Air: return kAir;
    case ThreatType::Experimental: return kExperimental;
    case ThreatType::Commander: return kCommander;
    case ThreatType::Artillery: return kArtillery;
    case ThreatType::AntiAir: return kAntiAir;
    case ThreatType::AntiSurface: return kAntiSurface;
    case ThreatType::AntiSub: return kAntiSub;
    case ThreatType::Economy: return kEconomy;
    case ThreatType::Unknown: break;
    }
    return kUnknown;
}

InfluenceMap::InfluenceMap(u32 map_width, u32 map_height, u32 army_count) {
    grid_ = std::max(32, static_cast<i32>(std::max(map_width, map_height)) / 16);
    width_ = static_cast<i32>(map_width) / grid_;
    height_ = static_cast<i32>(map_height) / grid_;
    if (width_ <= 0 || height_ <= 0) {
        width_ = height_ = 0;
        return;
    }
    cells_.resize(static_cast<size_t>(width_) * static_cast<size_t>(height_));
    for (auto& cell : cells_) cell.threats.resize(army_count);
}

i32 InfluenceMap::cell_of(const Vector3& pos) const {
    if (width_ <= 0 || height_ <= 0) return 0;
    const i32 x = std::clamp(static_cast<i32>(pos.x) / grid_, 0, width_ - 1);
    const i32 z = std::clamp(static_cast<i32>(pos.z) / grid_, 0, height_ - 1);
    return x + z * width_;
}

Vector3 InfluenceMap::cell_centre(i32 x, i32 z) const {
    // In whole units, as Moho reports them.
    const i32 cx = grid_ / 2 + x * grid_;
    const i32 cz = grid_ / 2 + z * grid_;
    return {static_cast<f32>(cx), 0.0f, static_cast<f32>(cz)};
}

CellRect InfluenceMap::cells_in(f32 x0, f32 z0, f32 x1, f32 z1) const {
    return {static_cast<i32>(x0) / grid_, static_cast<i32>(z0) / grid_,
            static_cast<i32>(x1) / grid_, static_cast<i32>(z1) / grid_};
}

void InfluenceMap::insert(u32 id, i32 source_army, const Vector3& pos, const ThreatSource& source) {
    if (cells_.empty()) return;
    const i32 cell = cell_of(pos);
    Entry entry;
    entry.source_army = source_army;
    entry.position = pos;
    entry.source = source;
    entry.decay = source.mobile ? kMobileDecay : 0.0f;
    entry.decay_ticks = kFreshTicks;
    cells_[static_cast<size_t>(cell)].entries[id] = entry;
    entry_cells_[id] = cell;
}

void InfluenceMap::report(u32 id, i32 source_army, const Vector3& pos, const ThreatSource& source) {
    const auto known = entry_cells_.find(id);
    if (known == entry_cells_.end()) {
        insert(id, source_army, pos, source);
        return;
    }
    if (known->second == cell_of(pos)) {
        auto& entry = cells_[static_cast<size_t>(known->second)].entries.at(id);
        entry.strength = 1.0f;
        entry.decay_ticks = kFreshTicks;
        entry.position = pos;
        return;
    }
    remove(id);
    insert(id, source_army, pos, source);
}

void InfluenceMap::remove(u32 id) {
    const auto known = entry_cells_.find(id);
    if (known == entry_cells_.end()) return;
    cells_[static_cast<size_t>(known->second)].entries.erase(id);
    entry_cells_.erase(known);
}

f32 InfluenceMap::strength(u32 id) const {
    const auto known = entry_cells_.find(id);
    return known == entry_cells_.end()
               ? 0.0f
               : cells_[static_cast<size_t>(known->second)].entries.at(id).strength;
}

void InfluenceMap::update(const std::function<bool(i32 army)>& allied_or_self,
                          const std::function<std::optional<UnitState>(u32 id)>& unit_state) {
    for (auto& cell : cells_) {
        // Scripts' threat fades; its Overall is the rest summed (Moho's
        // DecayInfluence).
        for (u8 lane = kStructures; lane < kLaneCount; ++lane)
            cell.assigned[lane] = decay_lane(cell.assigned[lane], cell.assigned_decay[lane]);
        auto& a = cell.assigned;
        a[kOverall] = a[kAntiSurface] + a[kExperimental] + a[kStructures] + a[kAntiSub] +
                      a[kCommander] + a[kNaval] + a[kEconomy] + a[kArtillery] + a[kAir] +
                      a[kUnknown] + a[kAntiAir] + a[kLand] + a[kStructuresNotMex];
        for (auto& lanes : cell.threats) lanes.fill(0.0f);

        for (auto it = cell.entries.begin(); it != cell.entries.end();) {
            Entry& entry = it->second;
            if (entry.decay_ticks > 0) --entry.decay_ticks;
            if (entry.decay_ticks == 0) entry.strength = decay_lane(entry.strength, entry.decay);
            if (entry.strength <= 0.0f) {
                entry_cells_.erase(it->first);
                it = cell.entries.erase(it);
                continue;
            }
            // What the army knows now of an enemy's unit that still exists:
            // its layer, and whether it has seen it closely (Moho reads the
            // recon flags; see the design's note on detail).
            if (!allied_or_self(entry.source_army)) {
                if (const auto state = unit_state(it->first)) {
                    entry.layer = state->layer;
                    if (state->detailed) entry.detailed = true;
                }
            }

            if (entry.source_army >= 0 &&
                static_cast<size_t>(entry.source_army) < cell.threats.size()) {
                Lanes& lanes = cell.threats[static_cast<size_t>(entry.source_army)];
                const ThreatSource& src = entry.source;
                const f32 s = entry.strength;
                const f32 anti_air = src.air * s;
                const f32 anti_surface = src.surface * s;
                const f32 anti_sub = src.sub * s;
                const f32 economy = src.economy * s;
                const f32 total = anti_air + anti_surface + anti_sub + economy;
                lanes[kOverall] += total;
                if (!src.mobile) {
                    lanes[kStructures] += total;
                    if (!src.mass_extractor) lanes[kStructuresNotMex] += total;
                } else if (src.flies) {
                    lanes[kAir] += total;
                } else if (entry.layer == ThreatLayer::Land) {
                    lanes[kLand] += total;
                } else if (entry.layer == ThreatLayer::Naval) {
                    lanes[kNaval] += total;
                }
                if (entry.detailed) {
                    // Moho's artillery category is looked up by a name no
                    // category has ("ARTILLERY, STRATEGIC"), so that lane
                    // stays 0 there as here.
                    if (src.experimental) lanes[kExperimental] += total;
                    if (src.commander) lanes[kCommander] += total;
                    lanes[kAntiAir] += anti_air;
                    lanes[kAntiSurface] += anti_surface;
                    lanes[kAntiSub] += anti_sub;
                    lanes[kEconomy] += economy;
                } else {
                    lanes[kUnknown] += total;
                }
            }
            ++it;
        }
    }
}

const InfluenceMap::Cell* InfluenceMap::cell_at(i32 x, i32 z) const {
    if (x < 0 || z < 0 || x >= width_ || z >= height_) return nullptr;
    return &cells_[static_cast<size_t>(x) + static_cast<size_t>(z) * static_cast<size_t>(width_)];
}

f32 InfluenceMap::cell_threat(i32 x, i32 z, ThreatType type, i32 army) const {
    const Cell* cell = cell_at(x, z);
    if (!cell) return 0.0f;
    const Lane lane = lane_of(type);
    f32 result = type == ThreatType::OverallNotAssigned ? 0.0f : cell->assigned[lane];
    if (army >= 0) {
        if (static_cast<size_t>(army) < cell->threats.size())
            result += cell->threats[static_cast<size_t>(army)][lane];
        return result;
    }
    for (const auto& lanes : cell->threats) result += lanes[lane];
    return result;
}

f32 InfluenceMap::threat_rect(i32 x, i32 z, i32 radius, const CellRect* on_map, ThreatType type,
                              i32 army) const {
    f32 total = 0.0f;
    for (i32 cz = z - radius; cz <= z + radius; ++cz) {
        if (cz < 0 || cz >= height_) continue;
        if (on_map && (cz < on_map->z0 || cz > on_map->z1)) continue;
        for (i32 cx = x - radius; cx <= x + radius; ++cx) {
            if (cx < 0 || cx >= width_) continue;
            if (on_map && (cx < on_map->x0 || cx > on_map->x1)) continue;
            total += cell_threat(cx, cz, type, army);
        }
    }
    return total;
}

f32 InfluenceMap::threat_between(const Vector3& a, const Vector3& b, const CellRect* on_map,
                                 ThreatType type, i32 army) const {
    if (width_ <= 0 || height_ <= 0) return 0.0f;
    const i32 from = cell_of(a);
    const i32 to = cell_of(b);
    i32 x0 = from % width_, z0 = from / width_;
    const i32 x1 = to % width_, z1 = to / width_;
    const i32 dx = std::abs(x1 - x0), dz = std::abs(z1 - z0);
    const i32 sx = x0 < x1 ? 1 : -1, sz = z0 < z1 ? 1 : -1;
    i32 err = dx - dz;
    f32 total = 0.0f;
    for (;;) {
        total += threat_rect(x0, z0, 0, on_map, type, army);
        if (x0 == x1 && z0 == z1) break;
        const i32 err2 = err * 2;
        if (err2 > -dz) {
            err -= dz;
            x0 += sx;
        }
        if (err2 < dx) {
            err += dx;
            z0 += sz;
        }
    }
    return total;
}

std::vector<InfluenceMap::CellThreat> InfluenceMap::threats_around(const Vector3& pos, i32 radius,
                                                                   const CellRect* on_map,
                                                                   ThreatType type,
                                                                   i32 army) const {
    std::vector<CellThreat> rows;
    if (width_ <= 0 || height_ <= 0) return rows;
    const i32 centre = cell_of(pos);
    const i32 x = centre % width_, z = centre / width_;
    for (i32 cz = z - radius; cz <= z + radius; ++cz) {
        if (cz < 0 || cz >= height_) continue;
        if (on_map && (cz < on_map->z0 || cz > on_map->z1)) continue;
        for (i32 cx = x - radius; cx <= x + radius; ++cx) {
            if (cx < 0 || cx >= width_) continue;
            if (on_map && (cx < on_map->x0 || cx > on_map->x1)) continue;
            const f32 threat = cell_threat(cx, cz, type, army);
            if (threat <= 0.0f) continue;
            const Vector3 c = cell_centre(cx, cz);
            rows.push_back({c.x, c.z, threat});
        }
    }
    // Highest first; equal threats keep the scan's order (Moho's sort leaves
    // their order unspecified).
    std::stable_sort(rows.begin(), rows.end(),
                     [](const CellThreat& l, const CellThreat& r) { return l.threat > r.threat; });
    return rows;
}

InfluenceMap::CellThreat InfluenceMap::highest_threat(i32 radius, const CellRect* on_map,
                                                      ThreatType type, i32 army,
                                                      const Vector3& start) const {
    // Moho starts from -200 with no cell, so a cell below that is never
    // chosen.
    CellThreat best{0.0f, 0.0f, -200.0f};
    f32 best_dist = std::numeric_limits<f32>::max();
    for (i32 z = 0; z < height_; ++z) {
        for (i32 x = 0; x < width_; ++x) {
            const f32 threat = radius != 0 ? threat_rect(x, z, radius, on_map, type, army)
                                           : cell_threat(x, z, type, army);
            const Vector3 c = cell_centre(x, z);
            const f32 dx = c.x - start.x, dz = c.z - start.z;
            const f32 dist = dx * dx + dz * dz;
            if (threat > best.threat || (threat == best.threat && dist < best_dist)) {
                best = {c.x, c.z, threat};
                best_dist = dist;
            }
        }
    }
    return best;
}

void InfluenceMap::assign_threat(const Vector3& pos, ThreatType type, f32 value, f32 rate) {
    if (cells_.empty() || type == ThreatType::OverallNotAssigned) return;
    if (rate < 0.0f) rate = 0.01f;
    Cell& cell = cells_[static_cast<size_t>(cell_of(pos))];
    const Lane lane = type == ThreatType::Overall ? kUnknown : lane_of(type);
    cell.assigned[lane] += value;
    cell.assigned_decay[lane] = cell.assigned[lane] * rate;
}

} // namespace osc::sim
