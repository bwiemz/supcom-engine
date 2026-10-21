"""Compares the original's unit blueprints, from a /synclog beat 0, with the engine's.

    python bp_diff.py beat00000.log opensupcom.log

The engine's log is a run with this folder's init_probes.lua, whose hook logs each blueprint's
fields as `PROBE bp <id> <path>=<value>`.

The original's beat 0 holds every blueprint as Moho filled it in, its defaults included. The
engine's are as its scripts see them. Only fields both have are compared: Moho's C++-only fields
never reach the scripts. Prints, per field, how many units differ and a few of them.
"""

import re
import sys
from collections import defaultdict

UNIT_ID = re.compile(r"^([a-z]{3}\d{4}):$")
INDEX = re.compile(r"\[\d+\]")


def parse_synclog(path: str) -> dict[str, dict[str, str]]:
    units: dict[str, dict[str, str]] = {}
    fields: dict[str, str] | None = None
    stack: list[tuple[int, str]] = []  # (indent, key)
    with open(path, encoding="latin-1") as f:
        for raw in f:
            line = raw.rstrip("\n")
            if line and not line[0].isspace():
                m = UNIT_ID.match(line)
                fields = units.setdefault(m[1], {}) if m else None
                stack = []
                continue
            if fields is None:
                continue
            indent = len(line) - len(line.lstrip(" "))
            text = line.strip()
            while stack and stack[-1][0] >= indent:
                _ = stack.pop()
            if text.endswith(":"):
                stack.append((indent, text[:-1]))
            elif stack:
                path_ = ".".join(k for _, k in stack).replace(".[", "[")
                fields[path_] = text
    return units


def parse_engine(path: str) -> dict[str, dict[str, str]]:
    units: dict[str, dict[str, str]] = defaultdict(dict)
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            at = line.find("PROBE bp ")
            if at >= 0 and "=" in line:
                _, _, uid, rest = line[at:].rstrip("\n").split(" ", 3)
                key, _, value = rest.partition("=")
                units[uid.lower()][key] = value
    return units


def same(a: str, b: str) -> bool:
    # beat 0 prints Moho's bools as true/false; the .bp's scripts hold 1/0.
    bools = {"true": "1", "false": "0"}
    a, b = bools.get(a.strip(), a), bools.get(b.strip(), b)
    try:
        return abs(float(a) - float(b)) <= 1e-4 * max(1.0, abs(float(a)))
    except ValueError:
        return a.strip().lower() == b.strip().lower()


def main() -> int:
    retail = parse_synclog(sys.argv[1])
    engine = parse_engine(sys.argv[2])
    common = sorted(retail.keys() & engine.keys())
    print(f"units: original {len(retail)}, engine {len(engine)}, both {len(common)}")
    only = sorted(retail.keys() - engine.keys())
    if only:
        print(f"  only in the original: {' '.join(only[:20])}")
    diffs: dict[str, list[str]] = defaultdict(list)
    compared = 0
    for uid in common:
        r, e = retail[uid], engine[uid]
        for key in r.keys() & e.keys():
            compared += 1
            if not same(r[key], e[key]):
                diffs[key].append(f"{uid}: {r[key]} | {e[key]}")
    print(f"fields compared: {compared}, differing: {sum(len(v) for v in diffs.values())}")
    for key, rows in sorted(diffs.items(), key=lambda kv: -len(kv[1])):
        print(f"== {key}: {len(rows)} units (original | engine)")
        for row in rows[:4]:
            print("  " + row)
    # Fields a .bp leaves out: Moho's default, which the engine's code must supply.
    omitted: dict[str, defaultdict[str, int]] = defaultdict(lambda: defaultdict(int))
    for uid in common:
        for key in retail[uid].keys() - engine[uid].keys():
            omitted[INDEX.sub("[]", key)][retail[uid][key]] += 1
    print("\nleft out of the .bp, Moho's value (units holding it / units leaving it out):")
    for key, values in sorted(omitted.items()):
        value, count = max(values.items(), key=lambda kv: kv[1])
        print(f"  {key} = {value}  ({count}/{sum(values.values())})")
    return 1 if diffs else 0


if __name__ == "__main__":
    sys.exit(main())
