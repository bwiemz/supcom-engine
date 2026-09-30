#include "sim/emitter_params.hpp"

#include <array>
#include <cctype>

namespace osc::sim {

namespace {

constexpr std::array<std::string_view, kEmitterParamCount> kParams = {"POSITION_X",
                                                                      "POSITION_Y",
                                                                      "POSITION_Z",
                                                                      "TICKCOUNT",
                                                                      "LIFETIME",
                                                                      "REPEATTIME",
                                                                      "TICKINCREMENT",
                                                                      "BLENDMODE",
                                                                      "FRAMECOUNT",
                                                                      "USE_LOCAL_VELOCITY",
                                                                      "USE_LOCAL_ACCELERATION",
                                                                      "USE_GRAVITY",
                                                                      "ALIGN_ROTATION",
                                                                      "INTERPOLATE_EMISSION",
                                                                      "TEXTURE_STRIPCOUNT",
                                                                      "ALIGN_TO_BONE",
                                                                      "SORTORDER",
                                                                      "FLAT",
                                                                      "SCALE",
                                                                      "LODCUTOFF",
                                                                      "EMITIFVISIBLE",
                                                                      "CATCHUPEMIT",
                                                                      "CREATEIFVISIBLE",
                                                                      "SNAPTOWATERLINE",
                                                                      "ONLYEMITONWATER",
                                                                      "PARTICLERESISTANCE"};

constexpr std::array<std::string_view, kEmitterCurves> kCurves = {
    "XDIR_CURVE",         "YDIR_CURVE",          "ZDIR_CURVE",      "EMITRATE_CURVE",
    "LIFETIME_CURVE",     "VELOCITY_CURVE",      "X_ACCEL_CURVE",   "Y_ACCEL_CURVE",
    "Z_ACCEL_CURVE",      "RESISTANCE_CURVE",    "SIZE_CURVE",      "X_POSITION_CURVE",
    "Y_POSITION_CURVE",   "Z_POSITION_CURVE",    "BEGINSIZE_CURVE", "ENDSIZE_CURVE",
    "ROTATION_CURVE",     "ROTATION_RATE_CURVE", "FRAMERATE_CURVE", "TEXTURESELECTION_CURVE",
    "RAMPSELECTION_CURVE"};

bool same(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::toupper(static_cast<unsigned char>(a[i])) != static_cast<unsigned char>(b[i]))
            return false;
    return true;
}

/// `name` without `prefix` when it starts with it (any case).
std::string_view unprefixed(std::string_view name, std::string_view prefix) {
    return name.size() > prefix.size() && same(name.substr(0, prefix.size()), prefix)
               ? name.substr(prefix.size())
               : name;
}

} // namespace

std::optional<u8> emitter_param(std::string_view name) {
    name = unprefixed(name, "EFFECT_");
    if (same(name, "POSITION")) return kParamPositionX;
    for (size_t i = 0; i < kParams.size(); ++i)
        if (same(name, kParams[i])) return static_cast<u8>(i);
    return std::nullopt;
}

std::string_view emitter_param_name(u8 param) {
    return param < kParams.size() ? kParams[param] : std::string_view{};
}

std::optional<u8> emitter_curve(std::string_view name) {
    name = unprefixed(name, "EMITTER_");
    for (size_t i = 0; i < kCurves.size(); ++i)
        if (same(name, kCurves[i])) return static_cast<u8>(i);
    return std::nullopt;
}

} // namespace osc::sim
