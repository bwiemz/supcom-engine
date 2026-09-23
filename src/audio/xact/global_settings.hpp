#pragma once

#include "core/result.hpp"
#include "core/types.hpp"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace osc::audio::xact {

/// XACT volumes and RPC volume values are millibels (100 mB = 1 dB);
/// -9600 mB is treated as silence.
inline constexpr f32 kSilenceMb = -9600.0f;

/// Amplitude ratio of a volume in millibels.
f32 millibels_to_gain(f32 mb);

/// A volume byte as the XACT builder writes it, in millibels (180 is 0).
f32 volume_byte_to_millibels(u8 b);

/// How a category or cue handles a play past its instance limit.
enum class LimitBehavior : u8 { Fail = 0, Queue = 1, ReplaceOldest = 2, ReplaceQuietest = 3, ReplaceLowestPriority = 4 };

struct Category {
    std::string name;
    u16 parent = 0xFFFF; ///< index, 0xFFFF for the root
    f32 volume_mb = 0;
    u8 instance_limit = 0xFF; ///< 0xFF: unlimited
    LimitBehavior limit_behavior = LimitBehavior::Fail;
    u16 fade_in_ms = 0;
    u16 fade_out_ms = 0;
};

struct Variable {
    std::string name;
    u8 accessibility = 0;
    f32 initial = 0, min = 0, max = 0;
    /// Global (engine-wide) rather than one value per cue instance.
    bool global() const { return (accessibility & 0x04) == 0; }
};

/// A runtime parameter control: a curve from a variable's value to a
/// parameter (volume in millibels for all of FA's).
struct RpcCurve {
    enum class Parameter : u16 { Volume = 0, Pitch = 1, ReverbSend = 2, FilterFrequency = 3, FilterQ = 4 };
    enum class PointType : u8 { Linear = 0, Fast = 1, Slow = 2, SinCos = 3 };
    struct Point {
        f32 x = 0, y = 0;
        PointType type = PointType::Linear;
    };

    u32 code = 0; ///< byte offset in the XGS: how sounds refer to it
    u16 variable = 0;
    Parameter parameter = Parameter::Volume;
    std::vector<Point> points;

    /// The parameter value at variable value `x` (clamped at the ends).
    f32 evaluate(f32 x) const;
};

/// XACT global settings (.xgs): categories, variables and RPC curves.
struct GlobalSettings {
    std::vector<Category> categories;
    std::vector<Variable> variables;
    std::vector<RpcCurve> rpcs;

    /// Index of the named category or variable, or -1.
    int find_category(std::string_view name) const;
    int find_variable(std::string_view name) const;
    /// The RPC a sound refers to by `code`, or nullptr.
    const RpcCurve* rpc(u32 code) const;

    static Result<GlobalSettings> parse(std::span<const u8> data);
};

} // namespace osc::audio::xact
