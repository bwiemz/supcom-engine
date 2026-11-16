#pragma once

#include "core/types.hpp"
#include "sim/entity_id.hpp"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace osc::renderer {

/// What a renderer keeps of each entity it draws from one update to the next,
/// in a vector ascending by id. An update asks for its entities in the
/// snapshot's order (ascending id), so each is found where the last one was
/// left, with no hashing, and the frames between two ticks, which ask for the
/// same entities, move nothing. Those an update didn't ask for (entities
/// gone) are dropped at finish().
template <typename T> class EntityCache {
public:
    /// Entity `id`'s entry this update: the one the last update left, else a
    /// new T{}. Asked again in the same update (a ghost of a live entity),
    /// the same entry. Valid until the next at() or finish().
    T& at(sim::EntityId id) {
        Entry* hit = nullptr;
        if (cursor_ < entries_.size() && entries_[cursor_].id == id) {
            hit = &entries_[cursor_];
        } else {
            // Past an entity gone since, at a new one, or out of step (the
            // ghosts, after the world's entities): searched from where the
            // walk is when ahead of it, else from the start.
            const size_t from = cursor_ > 0 && entries_[cursor_ - 1].id < id ? cursor_ : 0;
            const auto it = std::lower_bound(
                entries_.begin() + static_cast<std::ptrdiff_t>(from), entries_.end(), id,
                [](const Entry& e, sim::EntityId v) { return e.id < v; });
            cursor_ = static_cast<size_t>(it - entries_.begin());
            if (it != entries_.end() && it->id == id) hit = &*it;
        }
        if (hit) {
            ++cursor_;
            if (hit->asked != update_) {
                hit->asked = update_;
                ++asked_;
            }
            return hit->value;
        }
        return added(id);
    }

    /// Ends the update: keeps what it asked for, one each.
    void finish() {
        if (!added_.empty() || asked_ != entries_.size()) {
            if (sorted_ < added_.size())
                std::sort(added_.begin(), added_.end(),
                          [](const Entry& a, const Entry& b) { return a.id < b.id; });
            next_.clear();
            next_.reserve(asked_ + added_.size());
            auto add = added_.begin();
            for (const Entry& e : entries_) {
                if (e.asked != update_) continue;
                for (; add != added_.end() && add->id < e.id; ++add) next_.push_back(*add);
                next_.push_back(e);
            }
            next_.insert(next_.end(), add, added_.end());
            entries_.swap(next_);
            added_.clear();
        }
        cursor_ = 0;
        asked_ = 0;
        sorted_ = 0;
        if (++update_ == 0) {
            // (Wrapped: no entry may look asked by the next update.)
            for (Entry& e : entries_) e.asked = 0;
            update_ = 1;
        }
    }

    /// Forget every entity (the next map's).
    void clear() {
        entries_.clear();
        added_.clear();
        cursor_ = 0;
        asked_ = 0;
        sorted_ = 0;
    }

    /// The entities kept by the last finish().
    size_t size() const { return entries_.size(); }

private:
    struct Entry {
        sim::EntityId id{};
        u32 asked = 0; ///< the update that last asked for it
        T value{};
    };

    /// An entity the last update didn't keep: a new entry, unless this update
    /// made it already.
    T& added(sim::EntityId id) {
        if (sorted_ == added_.size() && (added_.empty() || added_.back().id < id)) {
            // Past every one made yet, as a walk in ascending id makes them
            ++sorted_;
        } else {
            const auto by_id = [](const Entry& e, sim::EntityId v) { return e.id < v; };
            const auto ascending_end = added_.begin() + static_cast<std::ptrdiff_t>(sorted_);
            const auto it = std::lower_bound(added_.begin(), ascending_end, id, by_id);
            if (it != ascending_end && it->id == id) return it->value;
            for (auto e = ascending_end; e != added_.end(); ++e)
                if (e->id == id) return e->value;
        }
        added_.push_back({id, update_, T{}});
        return added_.back().value;
    }

    std::vector<Entry> entries_; ///< the last update's, ascending id
    std::vector<Entry> added_;   ///< this update's new ones, as asked
    std::vector<Entry> next_;    ///< finish()'s merge, kept for its capacity
    size_t cursor_ = 0;          ///< into entries_: one past the last found
    size_t asked_ = 0;           ///< entries_ this update asked for
    size_t sorted_ = 0;          ///< added_'s leading run in ascending id
    u32 update_ = 1;             ///< this update's stamp
};

} // namespace osc::renderer
