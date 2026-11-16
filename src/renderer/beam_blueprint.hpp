#pragma once

#include "core/types.hpp"

#include <array>
#include <string>
#include <unordered_map>
#include <unordered_set>

struct lua_State;

namespace osc::blueprints {
class BlueprintStore;
}

namespace osc::renderer {

/// A `BeamBlueprint { ... }` (M214a), with Moho's defaults (RBeamBlueprint)
/// for what it leaves out.
struct BeamBlueprintData {
    std::string texture; ///< TextureName
    f32 length = 10.0f;  ///< how far a beam from one bone reaches
    f32 lifetime = 1.0f;
    f32 thickness = 1.0f;                       ///< half the strip's width
    f32 ushift = 0.0f, vshift = 0.0f;           ///< UV scroll per tick
    std::array<f32, 4> start_color{1, 1, 1, 0}; ///< RGBA at the start
    std::array<f32, 4> end_color{1, 1, 1, 0};   ///< and at the end
    f32 lod_cutoff = 200.0f;                    ///< drawn within this of the camera
    u8 fidelity = 0b111;                        ///< the fidelities it is made at
    f32 repeat_rate = 0.0f;                     ///< texture repeats per unit of length (0: once)
    i32 blendmode = 3;                          ///< particle.fx's TBeam suffix (3: ADD)
};

/// Beam blueprints by the name an effect gives, read as
/// EmitterBlueprintCache reads emitters: from the registered blueprint.
class BeamBlueprintCache {
public:
    /// The registered blueprints it reads (EmitterBlueprintCache's).
    void set_blueprints(const blueprints::BlueprintStore* store) { store_ = store; }

    /// The beam blueprint registered as `path`, read on first use; null if
    /// none is (an emitter's, say). A miss is remembered.
    const BeamBlueprintData* get(const std::string& path, lua_State* L);

    void clear() {
        cache_.clear();
        failed_.clear();
    }

private:
    const blueprints::BlueprintStore* store_ = nullptr;
    std::unordered_map<std::string, BeamBlueprintData> cache_;
    std::unordered_set<std::string> failed_;
};

} // namespace osc::renderer
