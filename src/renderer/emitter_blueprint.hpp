#pragma once

#include "core/types.hpp"

#include <string>
#include <unordered_map>
#include <vector>

struct lua_State;

namespace osc::renderer {

/// A single key in an emitter curve: time (x), value (y), variance (z).
struct CurveKey {
    f32 x = 0; // time along curve
    f32 y = 0; // value at this point
    f32 z = 0; // random variance (+/-)
};

/// A time-parameterized curve from an EmitterBlueprint.
struct EmitterCurve {
    f32 x_range = 0;              // total time range
    std::vector<CurveKey> keys;

    /// Sample the curve at time t (linearly interpolated between keys).
    /// Returns the y value (no randomness applied).
    f32 sample(f32 t) const;

    /// Sample with random variance: y + random(-z, +z).
    f32 sample_random(f32 t) const;
};

/// Parsed emitter blueprint data — cached per blueprint path.
struct EmitterBlueprintData {
    std::string blueprint_id;
    f32 lifetime = 1.0f;
    f32 repeattime = 1.0f;
    u32 texture_frame_count = 1;
    u32 texture_strip_count = 1;
    u32 blendmode = 0;           // 0=alpha, 3=additive
    f32 lod_cutoff = 300.0f;
    f32 sort_order = 0;

    bool local_velocity = false;
    bool local_acceleration = false;
    bool gravity = false;
    bool align_rotation = false;
    bool align_to_bone = false;
    bool flat = false;
    bool emit_if_visible = true;
    bool catchup_emit = true;
    bool snap_to_waterline = false;
    bool only_emit_on_water = false;
    bool interpolate_emission = true;

    std::string texture_path;
    std::string ramp_texture_path;

    // Emitter-level curves (parameterize emission over emitter lifetime)
    EmitterCurve emit_rate;
    EmitterCurve x_direction, y_direction, z_direction;
    EmitterCurve velocity;
    EmitterCurve x_accel, y_accel, z_accel;
    EmitterCurve x_pos, y_pos, z_pos;

    // Per-particle curves (parameterize particle properties at spawn)
    EmitterCurve lifetime_curve; // particle lifetime
    EmitterCurve start_size, end_size, size_curve;
    EmitterCurve initial_rotation, rotation_rate;
    EmitterCurve frame_rate;
    EmitterCurve texture_selection, ramp_selection;
};

/// Cache of parsed EmitterBlueprintData, keyed by VFS path.
class EmitterBlueprintCache {
public:
    /// Parse an emitter blueprint from a Lua table on the stack.
    /// The table should be the result of loading a .bp file.
    /// Returns cached data (or loads from VFS on first access).
    const EmitterBlueprintData* get(const std::string& bp_path, lua_State* L);

    void clear() { cache_.clear(); }

private:
    static EmitterBlueprintData parse_from_lua(lua_State* L, int table_idx);
    static EmitterCurve parse_curve(lua_State* L, int table_idx, const char* field_name);

    std::unordered_map<std::string, EmitterBlueprintData> cache_;
};

} // namespace osc::renderer
