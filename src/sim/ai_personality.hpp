#pragma once

#include "core/types.hpp"

#include <array>
#include <cmath>
#include <string>
#include <vector>

namespace osc::sim {

/// An AI brain's personality, as Moho's CAiPersonality (faf-re
/// CAiPersonality.cpp): the AverageJoe row of /lua/aipersonality.lua's
/// AIPersonalityTemplate, each value a range the difficulty picks a point
/// in. Moho reads it as a brain is made (ReadData) and never changes it.
struct AiPersonality {
    /// The row's ranges, in its order: its fields 3 to 33, less the two
    /// lists at 18 and 27.
    enum Value : u8 {
        ArmySize,
        PlatoonSize,
        AttackFrequency,
        RepeatAttackFrequency,
        CounterForces,
        IntelGathering,
        CoordinatedAttacks,
        ExpansionDriven,
        TechAdvancement,
        UpgradesDriven,
        DefenseDriven,
        EconomyDriven,
        FactoryTycoon,
        IntelBuildingTycoon,
        SuperWeaponTendency,
        AirUnitsEmphasis,
        TankUnitsEmphasis,
        BotUnitsEmphasis,
        SeaUnitsEmphasis,
        SpecialtyForcesEmphasis,
        SupportUnitsEmphasis,
        DirectDamageEmphasis,
        IndirectDamageEmphasis,
        SurvivalEmphasis,
        TeamSupport,
        FormationUse,
        TargetSpread,
        QuittingTendency,
        ChatFrequency,
        kValueCount
    };
    struct Range {
        f32 min = 0.0f;
        f32 max = 0.0f;
    };
    /// The row's field (from 1) a value is read from.
    static constexpr int field_of(Value v) {
        return v < AirUnitsEmphasis ? v + 3 : v < SurvivalEmphasis ? v + 4 : v + 5;
    }

    std::string name = "None";
    std::string chat = "None";
    std::array<Range, kValueCount> ranges{};
    std::vector<std::string> favourite_structures;
    std::vector<std::string> favourite_units;
    /// CAiPersonality's constructor's; nothing changes it.
    f32 difficulty = 0.5f;

    /// A value's point in its range: its min at difficulty 0, its max at 1.
    f32 value(Value v) const {
        const Range& r = ranges[v];
        return (1.0f - difficulty) * r.min + r.max * difficulty;
    }
    /// AdjustDelay(delay, spread): the delay, plus (1 - difficulty) of
    /// delay x spread rounded to nearest, as Moho's fistp rounds it.
    i32 adjust_delay(i32 delay, i32 spread) const {
        const f32 scaled = (1.0f - difficulty) * static_cast<f32>(delay * spread);
        return delay + static_cast<i32>(std::nearbyint(scaled));
    }
};

} // namespace osc::sim
