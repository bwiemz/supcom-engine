#pragma once

// The sim's C++ state as bytes, and back (M208c-b; design:
// docs/plans/2026-09-29-m208c-state-snapshots-design.md). A snapshot is
// taken between ticks and loaded into a sim booted for the same game: the
// load replaces everything a game changes, and rebuilds what follows from
// it (spatial grids, id lists, category bits, cache pointers). The Lua heap
// goes separately (lpersist.h), after the C++ objects it points at exist.
//
// Every field of every class is written in declaration order, bar the ones
// the class's section says are derived or the host's; a class carries
// `friend struct StateIO`. A new field joins its class's serializer.

#include "core/types.hpp"
#include "sim/entity.hpp"

#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace osc::sim {

class ArmyBrain;
class CategoryExpr;
class EconomyEventRegistry;
class EntityRegistry;
class IEffect;
class IEffectRegistry;
class InfluenceMap;
class Manipulator;
class Navigator;
class Platoon;
class Projectile;
class Prop;
class Shield;
class SimState;
class ThreadManager;
class TransportSlots;
class Unit;
class Weapon;
struct UnitCommand;
struct ScheduledCommand;
struct SimCallbackEntry;
class CommandScheduler;
namespace path {
class PathFinder;
class PathListener;
class PathNavigator;
class PathQueue;
class PathSearch;
class Traveler;
} // namespace path

/// Little-endian where it matters to no one: a snapshot is read by the
/// build that wrote it (floats as their bits, sizes as u32).
class StateWriter {
public:
    explicit StateWriter(std::vector<u8>& out) : b_(out) {}
    void raw(const void* p, size_t n) {
        const auto* c = static_cast<const u8*>(p);
        b_.insert(b_.end(), c, c + n);
    }
    template <typename T> void pod(const T& v) { raw(&v, sizeof v); }
    void u8v(u8 v) { pod(v); }
    void u16v(u16 v) { pod(v); }
    void u32v(u32 v) { pod(v); }
    void i32v(i32 v) { pod(v); }
    void u64v(u64 v) { pod(v); }
    void f32v(f32 v) { pod(v); }
    void f64v(f64 v) { pod(v); }
    void b(bool v) { u8v(v ? 1 : 0); }
    void size(size_t n) { u32v(static_cast<u32>(n)); }
    void str(std::string_view s) {
        size(s.size());
        raw(s.data(), s.size());
    }
    void vec3(const Vector3& v) {
        f32v(v.x);
        f32v(v.y);
        f32v(v.z);
    }
    void quat(const Quaternion& q) {
        f32v(q.x);
        f32v(q.y);
        f32v(q.z);
        f32v(q.w);
    }
    /// A section's start: a load checks it, so a serializer that wrote more
    /// or less than its reader reads fails where it happened.
    void tag(const char (&name)[5]) { raw(name, 4); }

private:
    std::vector<u8>& b_;
};

/// Reads what StateWriter wrote. The first failure is kept (with where it
/// happened) and every read after it yields zeros.
class StateReader {
public:
    explicit StateReader(const std::vector<u8>& in) : b_(in) {}
    bool ok() const { return error_.empty(); }
    const std::string& error() const { return error_; }
    void fail(std::string what) {
        if (error_.empty()) error_ = std::move(what) + " (at byte " + std::to_string(pos_) + ")";
    }
    bool at_end() const { return pos_ == b_.size(); }
    size_t left() const { return b_.size() - pos_; }

    void raw(void* p, size_t n) {
        if (!ok() || left() < n) {
            fail("a truncated snapshot");
            std::memset(p, 0, n);
            return;
        }
        std::memcpy(p, b_.data() + pos_, n);
        pos_ += n;
    }
    template <typename T> T pod() {
        T v{};
        raw(&v, sizeof v);
        return v;
    }
    u8 u8v() { return pod<u8>(); }
    u16 u16v() { return pod<u16>(); }
    u32 u32v() { return pod<u32>(); }
    i32 i32v() { return pod<i32>(); }
    u64 u64v() { return pod<u64>(); }
    f32 f32v() { return pod<f32>(); }
    f64 f64v() { return pod<f64>(); }
    bool b() { return u8v() != 0; }
    /// A count of things each at least `min_bytes` long: no more than the
    /// snapshot holds.
    size_t size(size_t min_bytes = 1) {
        const size_t n = u32v();
        if (n > left() / (min_bytes == 0 ? 1 : min_bytes)) {
            fail("a count past the snapshot's end");
            return 0;
        }
        return n;
    }
    std::string str() {
        const size_t n = size();
        std::string s(n, '\0');
        if (n) raw(s.data(), n);
        return s;
    }
    Vector3 vec3() {
        Vector3 v;
        v.x = f32v();
        v.y = f32v();
        v.z = f32v();
        return v;
    }
    Quaternion quat() {
        Quaternion q;
        q.x = f32v();
        q.y = f32v();
        q.z = f32v();
        q.w = f32v();
        return q;
    }
    void tag(const char (&name)[5]) {
        char got[4] = {};
        raw(got, 4);
        if (ok() && std::memcmp(got, name, 4) != 0)
            fail(std::string("section ") + name + " expected");
    }

private:
    const std::vector<u8>& b_;
    size_t pos_ = 0;
    std::string error_;
};

/// The serializers, one pair per class; the classes befriend it.
struct StateIO {
    // state_io_entities.cpp
    static void save(StateWriter& w, const Entity& e);
    static void load(StateReader& r, Entity& e);
    static void save(StateWriter& w, const Unit& u);
    static void load(StateReader& r, Unit& u, SimState& sim);
    static void save(StateWriter& w, const Projectile& p);
    static void load(StateReader& r, Projectile& p);
    static void save(StateWriter& w, const Prop& p);
    static void load(StateReader& r, Prop& p);
    static void save(StateWriter& w, const Shield& s);
    static void load(StateReader& r, Shield& s);
    static void save(StateWriter& w, const Navigator& n);
    static void load(StateReader& r, Navigator& n, SimState& sim, i32 army);
    // Moho pathing (state_io_paths.cpp)
    static void save(StateWriter& w, const path::PathFinder& f);
    static void load(StateReader& r, path::PathFinder& f, const SimState& sim,
                     path::PathListener* owner);
    static void save(StateWriter& w, const path::PathNavigator& n);
    static void load(StateReader& r, path::PathNavigator& n, SimState& sim, i32 army);
    static void save(StateWriter& w, const path::PathSearch& s);
    static void load(StateReader& r, path::PathSearch& s);
    static void save(StateWriter& w, const path::PathQueue& q, const SimState& sim);
    static void load(StateReader& r, path::PathQueue& q, SimState& sim);
    static void save_path_maps(StateWriter& w, const SimState& sim);
    /// The unit whose navigator's finder `t` is (0: none), and back.
    static u32 traveler_id(const SimState& sim, const path::Traveler* t);
    static path::PathFinder* traveler_of(SimState& sim, u32 id);
    static void load_path_maps(StateReader& r, SimState& sim);
    static void save(StateWriter& w, const Weapon& wp);
    static void load(StateReader& r, Weapon& wp);
    static void save(StateWriter& w, const UnitCommand& c);
    static void load(StateReader& r, UnitCommand& c);
    static void save(StateWriter& w, const CategoryExpr& c);
    static void load(StateReader& r, CategoryExpr& c, int depth = 0);
    /// A manipulator with its kind first; load makes one of that kind.
    static void save(StateWriter& w, const Manipulator& m);
    static std::unique_ptr<Manipulator> load_manipulator(StateReader& r, Unit& owner,
                                                         SimState& sim);

    // state_io_world.cpp
    static void save(StateWriter& w, const ArmyBrain& a, const SimState& sim);
    static void load(StateReader& r, ArmyBrain& a, SimState& sim);
    static void save(StateWriter& w, const Platoon& p);
    static void load(StateReader& r, Platoon& p);
    static void save(StateWriter& w, const InfluenceMap& m);
    static void load(StateReader& r, InfluenceMap& m);
    static void save(StateWriter& w, const IEffectRegistry& fx);
    static void load(StateReader& r, IEffectRegistry& fx);
    static void save(StateWriter& w, const EconomyEventRegistry& ev);
    static void load(StateReader& r, EconomyEventRegistry& ev);
    static void save(StateWriter& w, const ThreadManager& t);
    static void load(StateReader& r, ThreadManager& t);
    static void save(StateWriter& w, const CommandScheduler& s);
    static void load(StateReader& r, CommandScheduler& s);
    static void save(StateWriter& w, const ScheduledCommand& c);
    static void load(StateReader& r, ScheduledCommand& c);
    static void save(StateWriter& w, const SimCallbackEntry& c);
    static void load(StateReader& r, SimCallbackEntry& c);

    // state_io.cpp
    static void save(StateWriter& w, const EntityRegistry& reg);
    static void load(StateReader& r, EntityRegistry& reg, SimState& sim);
    static void save(StateWriter& w, const SimState& sim);
    static void load(StateReader& r, SimState& sim);

    // sim_snapshot.cpp (sim_snapshot.hpp's)
    static std::string save_snapshot(SimState& sim, std::vector<u8>& out);
    static std::string load_snapshot(SimState& sim, const std::vector<u8>& in);
};

/// The sim's C++ state, between ticks.
std::vector<u8> save_sim_state(const SimState& sim);
/// Replace a sim booted for the same game with a saved one. Empty on
/// success, else what failed; on a failure the sim must be discarded.
std::string load_sim_state(SimState& sim, const std::vector<u8>& bytes);

} // namespace osc::sim
