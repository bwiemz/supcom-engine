"""Compares the drive grid probe's two logs, the original's and the engine's.

    python drive_diff.py retail.log opensupcom.log [--trace]

For each unit and leg it measures: the tick its turn starts and the tick it starts to move, its
top speed, the tick its order ends, how far from the goal it comes to rest, how far it coasts
after its order ends, and whether it backs up. It prints, per measure, how many units differ
beyond a small tolerance, those units grouped by the blueprint fields their drive reads
(MotionType, RotateOnSpot, whether TurnRadius and BackUpDistance are given), and the worst
few. Exit 1 if any differ.

With --trace it compares the paths tick by tick instead: per leg, the largest distance between
the original's unit and the engine's at the same tick, and how those spread over all legs. Where
both logs print positions with 9 significant digits (enough to give a float back bit for bit), it
also counts the legs that match bit for bit, and how far into the others the first bit differs.
"""

import math
import re
import struct
import sys
from collections import defaultdict
from itertools import pairwise

ZS = 117.0  # the probe's start line (its ScenarioUtilities.lua)
GOAL_Z = {1: ZS - 40, 2: ZS - 10, 3: ZS - 13, 4: ZS - 7}
SAMPLE = re.compile(r"PROBE dg b=\d+ (\w+) L(\d) t=(\d+) (\S+) (\S+) (\S+) ([m-])")
LANE = re.compile(r"PROBE dglane b=\d+ (\w+) x=(\S+)")
TRAITS = re.compile(r"PROBE dgbp (\w+) (.*)")

# measure: (tolerance, whether it is a tick count)
MEASURES = {
    "turn": (1, True),
    "side": (0, True),  # the way its first turn goes: -1, 0 or 1
    "move": (1, True),
    "top": (0.03, False),  # relative
    "done": (2, True),
    "miss": (0.15, False),
    "coast": (0.15, False),
    "backs": (0, True),
}

Samples = list[tuple[int, float, float, float, bool]]


def parse(path: str):
    runs: dict[tuple[str, int], Samples] = defaultdict(list)
    lanes: dict[str, float] = {}
    traits: dict[str, dict[str, str]] = {}
    with open(path, encoding="latin-1") as f:
        for line in f:
            if m := SAMPLE.search(line):
                runs[(m[1], int(m[2]))].append(
                    (int(m[3]), float(m[4]), float(m[5]), float(m[6]), m[7] == "m")
                )
            elif m := LANE.search(line):
                lanes[m[1]] = float(m[2])
            elif m := TRAITS.search(line):
                traits[m[1]] = dict(kv.split("=", 1) for kv in m[2].split())
    return runs, lanes, traits


def measure(s: Samples, lane_x: float, leg: int) -> dict[str, float]:
    t0, x0, z0, yaw0, _ = s[0]
    turn = next((t for t, _, _, y, _ in s if abs((y - yaw0 + 180) % 360 - 180) > 0.5), -1)
    # moving: 0.3 off its start, past the lurch a unit at rest can give on an order
    move = next((t for t, x, z, _, _ in s if math.hypot(x - x0, z - z0) > 0.3), -1)
    first = next((y for _, _, _, y, _ in s if abs((y - yaw0 + 180) % 360 - 180) > 0.5), yaw0)
    side = math.copysign(1.0, (first - yaw0 + 180) % 360 - 180) if first != yaw0 else 0.0
    top = max((math.hypot(b[1] - a[1], b[2] - a[2]) * 10 for a, b in pairwise(s)), default=0.0)
    done = next((t for t, _, _, _, m in s if t > max(move, 0) and not m), -1)
    tx, tz, ty = s[-1][1], s[-1][2], s[-1][3]
    at_done = next(((x, z) for t, x, z, _, _ in s if t == done), (tx, tz))
    # backing up: it ends behind where it started along the way it faces
    fx, fz = math.sin(math.radians(ty)), math.cos(math.radians(ty))
    backs = 1.0 if (tx - x0) * fx + (tz - z0) * fz < -0.05 else 0.0
    return {
        "turn": turn - t0 if turn >= 0 else -1,
        "side": side,
        "move": move - t0 if move >= 0 else -1,
        "top": top,
        "done": done - t0 if done >= 0 else -1,
        "miss": math.hypot(tx - lane_x, tz - GOAL_Z[leg]),
        "coast": math.hypot(tx - at_done[0], tz - at_done[1]),
        "backs": backs,
    }


def differs(name: str, a: float, b: float) -> bool:
    tol, _ = MEASURES[name]
    if name == "top":
        return abs(a - b) > tol * max(abs(a), 1e-6)
    return abs(a - b) > tol


def trace_gap(r: Samples, e: Samples) -> float:
    at = {t: (x, z) for t, x, z, _, _ in e}
    return max(
        (math.hypot(x - at[t][0], z - at[t][1]) for t, x, z, _, _ in r if t in at), default=0.0
    )


def f32(v: float) -> float:
    """The float a printed number gives back. The two C runtimes break a printing tie apart
    (640.3515625 prints 640.351563 in the original and 640.351562 in the engine)."""
    return struct.unpack("f", struct.pack("f", v))[0]


def first_difference(r: Samples, e: Samples) -> int | None:
    """The first tick the two positions differ at all, from the leg's start; None if never."""
    at = {t: (f32(x), f32(z)) for t, x, z, _, _ in e}
    t0 = r[0][0] if r else 0
    return next((t - t0 for t, x, z, _, _ in r if t in at and (f32(x), f32(z)) != at[t]), None)


def trace(r_runs: dict[tuple[str, int], Samples], e_runs: dict[tuple[str, int], Samples]) -> int:
    keys = sorted(r_runs.keys() & e_runs.keys())
    gaps = sorted((trace_gap(r_runs[k], e_runs[k]), k) for k in keys)
    if not gaps:
        return 1
    print(f"legs: {len(gaps)}; largest gap from the original along each leg:")
    for share in (0.5, 0.75, 0.9, 1.0):
        gap, _ = gaps[min(len(gaps) - 1, int(share * len(gaps)))]
        print(f"  {int(share * 100):3d}% of legs within {gap:.3f}")
    for limit in (0.01, 0.05, 0.1, 0.5):
        print(f"  within {limit}: {sum(g <= limit for g, _ in gaps)}")
    firsts = [first_difference(r_runs[k], e_runs[k]) for k in keys]
    differing = sorted(f for f in firsts if f is not None)
    print(f"  bit for bit: {len(firsts) - len(differing)}")
    if differing:
        median, earliest = differing[len(differing) // 2], differing[0]
        print(f"  the others first differ at tick {median} (median), {earliest} at the earliest")
    for gap, (uid, leg) in gaps[-8:][::-1]:
        print(f"    {uid} L{leg}: {gap:.3f}")
    return 0


def group_of(tr: dict[str, str]) -> str:
    def given(key: str) -> str:
        return "set" if float(tr.get(key, "-1")) >= 0 else "unset"

    return (
        f"{tr.get('mt', '?').replace('RULEUMT_', '')} ros={tr.get('ros', '?')} "
        f"rad={given('rad')} bud={given('bud')}"
    )


def main() -> int:
    args = [a for a in sys.argv[1:] if a != "--trace"]
    r_runs, r_lanes, traits = parse(args[0])
    e_runs, e_lanes, _ = parse(args[1])
    if "--trace" in sys.argv:
        return trace(r_runs, e_runs)
    keys = sorted(r_runs.keys() & e_runs.keys())
    print(f"runs: original {len(r_runs)}, engine {len(e_runs)}, both {len(keys)}")
    diffs: dict[str, list[tuple[float, str, int, float, float]]] = defaultdict(list)
    for uid, leg in keys:
        r = measure(r_runs[(uid, leg)], r_lanes.get(uid, 0.0), leg)
        e = measure(e_runs[(uid, leg)], e_lanes.get(uid, 0.0), leg)
        for name in MEASURES:
            if differs(name, r[name], e[name]):
                diffs[name].append((abs(r[name] - e[name]), uid, leg, r[name], e[name]))
    for name in MEASURES:
        found = diffs[name]
        print(f"\n== {name}: {len(found)} of {len(keys)} unit-legs differ")
        groups: dict[str, int] = defaultdict(int)
        for _, uid, leg, _, _ in found:
            groups[f"L{leg} {group_of(traits.get(uid, {}))}"] += 1
        for g, n in sorted(groups.items(), key=lambda kv: -kv[1])[:8]:
            print(f"  {n:4d}  {g}")
        for _, uid, leg, rv, ev in sorted(found, reverse=True)[:4]:
            print(f"    {uid} L{leg}: original {rv:.2f}, engine {ev:.2f}")
    return 1 if any(diffs.values()) else 0


if __name__ == "__main__":
    sys.exit(main())
