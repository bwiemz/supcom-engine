#pragma once

// Moho's emitter parameters and curves by name (M214d): EEmitterParam and
// EEmitterCurve, as IEffect:SetEmitterParam, SetEmitterCurveParam and
// ResizeEmitterCurve name them from Lua -- in any case, with or without the
// "EFFECT_"/"EMITTER_" prefix (gpg's enum lexicals).

#include "core/types.hpp"

#include <optional>
#include <string_view>

namespace osc::sim {

/// EEmitterParam (faf-re render/EEmitterParam.h).
enum EmitterParam : u8 {
    kParamPositionX = 0, ///< also "POSITION"
    kParamPositionY = 1,
    kParamPositionZ = 2,
    kParamTickCount = 3,
    kParamLifetime = 4,
    kParamRepeatTime = 5,
    kParamTickIncrement = 6,
    kParamBlendMode = 7,
    kParamFrameCount = 8,
    kParamUseLocalVelocity = 9,
    kParamUseLocalAcceleration = 10,
    kParamUseGravity = 11,
    kParamAlignRotation = 12,
    kParamInterpolateEmission = 13,
    kParamTextureStripCount = 14,
    kParamAlignToBone = 15,
    kParamSortOrder = 16,
    kParamFlat = 17,
    kParamScale = 18,
    kParamLodCutoff = 19,
    kParamEmitIfVisible = 20,
    kParamCatchupEmit = 21,
    kParamCreateIfVisible = 22,
    kParamSnapToWaterline = 23,
    kParamOnlyEmitOnWater = 24,
    kParamParticleResistance = 25,
    kEmitterParamCount = 26
};

/// The parameter `name` names, or nothing.
std::optional<u8> emitter_param(std::string_view name);
/// Its canonical name ("LIFETIME"), as the effect's params keep it.
std::string_view emitter_param_name(u8 param);

/// EEmitterCurve, in the emitter blueprint's curve order (renderer's
/// EmitterCurveId); 21 of them.
constexpr u8 kEmitterCurves = 21;
/// The curve `name` names ("X_POSITION_CURVE"), or nothing.
std::optional<u8> emitter_curve(std::string_view name);

} // namespace osc::sim
