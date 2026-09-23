#include "sim/game_setup.hpp"

#include "sim/command_codec.hpp"

namespace osc::sim {

namespace {

void write_int(ByteWriter& w, int v) {
    w.u32v(static_cast<u32>(v));
}
int read_int(ByteReader& r) {
    return static_cast<int>(r.u32v());
}

} // namespace

void write_game_setup(ByteWriter& w, const GameSetup& s) {
    w.str(s.scenario);
    w.u64v(s.seed);
    write_int(w, s.army_count);

    w.u32v(static_cast<u32>(s.slots.size()));
    for (const auto& slot : s.slots) {
        w.u8v(slot.configured ? 1 : 0);
        w.u8v(slot.human ? 1 : 0);
        write_int(w, slot.faction);
        write_int(w, slot.team);
        write_int(w, slot.start_spot);
        write_int(w, slot.player_color);
        write_int(w, slot.army_color);
        write_int(w, slot.handicap);
        w.str(slot.ai_personality);
    }

    w.u8v(s.options.configured ? 1 : 0);
    w.u32v(static_cast<u32>(s.options.values.size()));
    for (const auto& [key, value] : s.options.values) {
        w.str(key);
        w.u8v(static_cast<u8>(value.type));
        switch (value.type) {
        case GameOptionValue::Type::String: w.str(value.string_value); break;
        case GameOptionValue::Type::Number: w.f64v(value.number_value); break;
        case GameOptionValue::Type::Boolean: w.u8v(value.bool_value ? 1 : 0); break;
        }
    }
    w.u32v(static_cast<u32>(s.options.restricted_categories.size()));
    for (const auto& category : s.options.restricted_categories) w.str(category);

    w.u32v(static_cast<u32>(s.ai_armies.size()));
    for (int army : s.ai_armies) write_int(w, army);
    w.str(s.ai_personality);
    w.f64v(s.cheat_mult);
    w.f64v(s.build_mult);
}

bool read_game_setup(ByteReader& r, GameSetup& s) {
    s = GameSetup{};
    s.scenario = r.str();
    s.seed = r.u64v();
    s.army_count = read_int(r);

    // Counts come from the file: read until they are met or the bytes run
    // out, never reserving for them up front.
    const u32 slots = r.u32v();
    for (u32 i = 0; i < slots && r.ok(); ++i) {
        ArmySlotConfig slot;
        slot.configured = r.u8v() != 0;
        slot.human = r.u8v() != 0;
        slot.faction = read_int(r);
        slot.team = read_int(r);
        slot.start_spot = read_int(r);
        slot.player_color = read_int(r);
        slot.army_color = read_int(r);
        slot.handicap = read_int(r);
        slot.ai_personality = r.str();
        s.slots.push_back(std::move(slot));
    }

    s.options.configured = r.u8v() != 0;
    const u32 values = r.u32v();
    for (u32 i = 0; i < values && r.ok(); ++i) {
        std::string key = r.str();
        GameOptionValue value;
        switch (r.u8v()) {
        case 0:
            value.type = GameOptionValue::Type::String;
            value.string_value = r.str();
            break;
        case 1:
            value.type = GameOptionValue::Type::Number;
            value.number_value = r.f64v();
            break;
        case 2:
            value.type = GameOptionValue::Type::Boolean;
            value.bool_value = r.u8v() != 0;
            break;
        default: r.fail(); break;
        }
        s.options.values.emplace_back(std::move(key), std::move(value));
    }
    const u32 restricted = r.u32v();
    for (u32 i = 0; i < restricted && r.ok(); ++i)
        s.options.restricted_categories.push_back(r.str());

    const u32 ai = r.u32v();
    for (u32 i = 0; i < ai && r.ok(); ++i) s.ai_armies.push_back(read_int(r));
    s.ai_personality = r.str();
    s.cheat_mult = r.f64v();
    s.build_mult = r.f64v();
    return r.ok();
}

} // namespace osc::sim
