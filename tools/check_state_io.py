#!/usr/bin/env python3
"""Fail on a sim field no snapshot serializer names (M208c).

A snapshot (src/sim/state_io*.cpp, sim_snapshot.cpp) writes every field of
the sim's classes, bar the derived and the host's ones, which its serializer
names in a comment with why. A field added later and forgotten is a silent
desync after a load, so this check reads each serialized class's data
members from its header and fails on any the serializers never mention.

It is a textual check: a member counts as covered when its name appears as
a word in the serializers' sources (in code, or in a comment that leaves it
out on purpose).

Usage: check_state_io.py <repo root>   (exit 1 on any field not covered)
       check_state_io.py --self-test
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

# (header, type): the classes and structs the serializers write field by
# field. Nested types are named Outer::Inner.
TYPES: list[tuple[str, str]] = [
    ("src/sim/entity.hpp", "Entity"),
    ("src/sim/entity.hpp", "Entity::BeamSetup"),
    ("src/sim/entity.hpp", "CollisionShape"),
    ("src/sim/unit.hpp", "Unit"),
    ("src/sim/unit.hpp", "UnitEconomy"),
    ("src/sim/unit.hpp", "StagingRules"),
    ("src/sim/unit.hpp", "IntelState"),
    ("src/sim/unit.hpp", "Unit::Drive"),
    ("src/sim/unit.hpp", "Unit::SiloBuild"),
    ("src/sim/unit.hpp", "Unit::StoragePlace"),
    ("src/sim/unit.hpp", "Unit::ScriptTaskRun"),
    ("src/sim/unit.hpp", "Unit::CarrierLanding"),
    ("src/sim/unit.hpp", "Unit::UnitBuiltCallback"),
    ("src/sim/unit_command.hpp", "UnitCommand"),
    ("src/sim/navigator.hpp", "Navigator"),
    ("src/sim/navigator.hpp", "Navigator::Collision"),
    ("src/sim/weapon.hpp", "Weapon"),
    ("src/sim/category_expr.hpp", "CategoryExpr"),
    ("src/sim/waitable.hpp", "Waitable"),
    ("src/sim/manipulator.hpp", "Manipulator"),
    ("src/sim/manipulator.hpp", "RotateManipulator"),
    ("src/sim/manipulator.hpp", "AnimManipulator"),
    ("src/sim/manipulator.hpp", "SlideManipulator"),
    ("src/sim/manipulator.hpp", "AimManipulator"),
    ("src/sim/manipulator.hpp", "SlaverManipulator"),
    ("src/sim/manipulator.hpp", "CollisionDetectorManipulator"),
    ("src/sim/manipulator.hpp", "FootPlantManipulator"),
    ("src/sim/manipulator.hpp", "StorageManipulator"),
    ("src/sim/transport_slots.hpp", "TransportLayout"),
    ("src/sim/transport_slots.hpp", "TransportSlots"),
    ("src/sim/transport_slots.hpp", "TransportSlots::Slot"),
    ("src/sim/projectile.hpp", "Projectile"),
    ("src/sim/prop.hpp", "Prop"),
    ("src/sim/shield.hpp", "Shield"),
    ("src/sim/army_brain.hpp", "ResourceState"),
    ("src/sim/army_brain.hpp", "ArmyBrain"),
    ("src/sim/platoon.hpp", "Platoon"),
    ("src/sim/influence_map.hpp", "ThreatSource"),
    ("src/sim/influence_map.hpp", "InfluenceMap"),
    ("src/sim/influence_map.hpp", "InfluenceMap::Entry"),
    ("src/sim/influence_map.hpp", "InfluenceMap::Cell"),
    ("src/sim/ieffect.hpp", "IEffect"),
    ("src/sim/ieffect.hpp", "IEffectRegistry"),
    ("src/sim/decal.hpp", "DecalSpec"),
    ("src/sim/economy_event.hpp", "EconomyEvent"),
    ("src/sim/economy_event.hpp", "EconomyEventRegistry"),
    ("src/sim/thread_manager.hpp", "ThreadEntry"),
    ("src/sim/thread_manager.hpp", "ThreadManager"),
    ("src/sim/command_scheduler.hpp", "ScheduledCommand"),
    ("src/sim/command_scheduler.hpp", "CommandScheduler"),
    ("src/sim/sim_callback_queue.hpp", "SimCallbackEntry"),
    ("src/sim/entity_registry.hpp", "EntityRegistry"),
    ("src/sim/sim_state.hpp", "SimState"),
    ("src/sim/sim_state.hpp", "SimState::EntityIntel"),
    ("src/sim/sim_state.hpp", "SimState::TempVision"),
    ("src/sim/sim_state.hpp", "BlipSnapshot"),
    ("src/sim/sim_state.hpp", "ResourceDeposit"),
    ("src/sim/armor_definition.hpp", "ArmorDefinition"),
    ("src/sim/intel_sources.hpp", "IntelHandle"),
    ("src/sim/intel_sources.hpp", "PaintedIntel"),
]

SERIALIZERS = [
    "src/sim/state_io.cpp",
    "src/sim/state_io_entities.cpp",
    "src/sim/state_io_world.cpp",
    "src/sim/sim_snapshot.cpp",
]

_COMMENT = re.compile(r"//[^\n]*|/\*.*?\*/", re.DOTALL)
_WORD = re.compile(r"[A-Za-z_]\w*")


def strip_comments(text: str) -> str:
    return _COMMENT.sub(lambda m: "\n" * m.group(0).count("\n"), text)


def find_block(text: str, name: str) -> tuple[int, int] | None:
    """The body of `struct/class name {...}` (Outer::Inner: inside Outer's),
    as (start after the brace, end at the matching brace)."""
    lo, hi = 0, len(text)
    for part in name.split("::"):
        m = re.compile(r"\b(?:class|struct)\s+" + re.escape(part) + r"\b[^;{]*\{").search(
            text, lo, hi
        )
        if not m:
            return None
        depth, i = 1, m.end()
        while i < hi and depth:
            if text[i] == "{":
                depth += 1
            elif text[i] == "}":
                depth -= 1
            i += 1
        lo, hi = m.end(), i - 1
    return lo, hi


def split_top(s: str, sep: str) -> list[str]:
    """`s` split on `sep` outside (), <>, [] and {}."""
    parts: list[str] = []
    cur: list[str] = []
    depth = 0
    for c in s:
        if c in "(<[{":
            depth += 1
        elif c in ")>]}":
            depth -= 1
        if c == sep and depth == 0:
            parts.append("".join(cur))
            cur = []
        else:
            cur.append(c)
    parts.append("".join(cur))
    return parts


def members(body: str) -> list[tuple[str, int]]:
    """The data members declared directly in a type's body, with the line
    (from the body's start) each is on."""
    out: list[tuple[str, int]] = []
    depth, start, line = 0, 0, 0
    statements: list[tuple[str, int]] = []
    for i, c in enumerate(body):
        if c == "\n":
            line += 1
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                # a nested type's or a function's body: its statement ends here
                statements.append((body[start : i + 1] + "{}", line))
                start = i + 1
        elif c == ";" and depth == 0:
            statements.append((body[start:i], line))
            start = i + 1
        elif c == ":" and depth == 0:
            label = body[start:i].strip()
            if label in ("public", "private", "protected"):
                start = i + 1
    for stmt, at in statements:
        s = " ".join(stmt.split())
        if not s or "{}" in s.replace(" ", "")[-2:] and "(" in s:
            continue
        head = s.split(" ", 1)[0]
        if head in ("using", "friend", "typedef", "static", "enum", "struct", "class", "template"):
            continue
        if s.endswith("{}") and ("struct" in s or "class" in s or "enum" in s or "(" in s):
            continue
        # A function: parentheses outside template arguments before any '='
        decl = split_top(s, "=")[0]
        flat = re.sub(r"<[^<>]*>", "", re.sub(r"<[^<>]*>", "", re.sub(r"<[^<>]*>", "", decl)))
        if "(" in flat or "operator" in flat or flat.startswith("~"):
            continue
        for k, part in enumerate(split_top(s, ",")):
            name_part = split_top(split_top(part, "=")[0], "{")[0].strip()
            words = _WORD.findall(name_part)
            if words and (k > 0 or len(words) > 1):
                out.append((words[-1], at))
    return out


def check(root: Path) -> list[str]:
    io_text = "\n".join((root / f).read_text() for f in SERIALIZERS)
    io_words = set(_WORD.findall(io_text))
    problems: list[str] = []
    for header, type_name in TYPES:
        text = strip_comments((root / header).read_text())
        block = find_block(text, type_name)
        if block is None:
            problems.append(f"{header}: {type_name} not found (update tools/check_state_io.py)")
            continue
        base_line = text.count("\n", 0, block[0]) + 1
        for name, at in members(text[block[0] : block[1]]):
            if name not in io_words:
                problems.append(
                    f"{header}:{base_line + at}: {type_name}::{name} is in no snapshot serializer"
                    + " (src/sim/state_io*.cpp): write it, or name it in its class's serializer"
                    + " with why it isn't"
                )
    return problems


def self_test() -> int:
    body = """
    struct Inner { int x_; };
    int a_ = 1;
    std::vector<std::pair<int, int>> b_;
    f32 c_ = 1.0f, d_ = 2.0f;
    Vector3 e_{};
    std::function<void(int)> f_;
    void method(int x);
    int getter() const { return a_; }
    static constexpr int K = 3;
    enum class E : u8 { A, B };
  private:
    u8 r_ = 255, g_ = 255;
    """
    got = [n for n, _ in members(body)]
    want = ["a_", "b_", "c_", "d_", "e_", "f_", "r_", "g_"]
    if got != want:
        print(f"self-test: members() gave {got}, want {want}")
        return 1
    nested = "class Outer { struct Inner { int y_; int z_ = 0; }; int w_; };"
    block = find_block(nested, "Outer::Inner")
    if block is None or [n for n, _ in members(nested[block[0] : block[1]])] != ["y_", "z_"]:
        print("self-test: nested types")
        return 1
    print("self-test: ok")
    return 0


def main(argv: list[str]) -> int:
    if argv == ["--self-test"]:
        return self_test()
    if len(argv) != 1:
        print(__doc__, file=sys.stderr)
        return 2
    problems = check(Path(argv[0]))
    for p in problems:
        print(p)
    if problems:
        return 1
    print(f"state_io: every field of {len(TYPES)} types is in a serializer")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
