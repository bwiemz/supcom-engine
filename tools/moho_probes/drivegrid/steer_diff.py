"""Compares the steering decisions of the drive grid probe's two runs, the original's and the engine's.

    python steer_diff.py retail.log beats_dir engine.log

The original writes its steering's decisions to its /synclog beat files (Sim::Logf): the waypoints
its navigator sets, each collision it resolves and how (it collides, within the cone or not), and
each push. The engine writes the same to its --steer-log file. This reads both, names each unit by
its blueprint (the original's by the probe's lane it starts on), counts every kind of event per
unit, and prints, per unit and leg, the first event where the two differ, ticks counted from the
batch's start.
"""

import re
import sys
from collections import Counter, defaultdict
from pathlib import Path

BATCH = re.compile(r"PROBE dg batch=(\d+) start tick=(\d+)")
LANE = re.compile(r"PROBE dglane b=(\d+) (\w+) x=(\S+)")
ZS = 117.0  # the probe's start line

STEERING = re.compile(r"^(0x[0-9a-f]{8})'s steering tick")
NAV_FOR = re.compile(r"in UpdateCurrentPosition\(\) for (0x[0-9a-f]{8})")
CURRENT = re.compile(r"currentpos=<(\S+),(\S+)>")
WAYPOINTS = re.compile(r"(0x[0-9a-f]{8})->SetWaypoints\((\d+)(?:, <(\S+),(\S+)>)?")
RESOLVE = re.compile(r"ResolvePossibleCollision\((0x[0-9a-f]{8}) @ .*?, (0x[0-9a-f]{8}) @")
ENGINE = re.compile(r"STEER t=(\d+) (\w+) (.*?)(?: #\d+)?$")

Event = tuple[int, str]  # (ticks from the batch's start, what)


def batches(log: str) -> tuple[dict[int, int], dict[int, dict[str, float]]]:
    starts: dict[int, int] = {}
    lanes: dict[int, dict[str, float]] = defaultdict(dict)
    with open(log, encoding="latin-1") as f:
        for line in f:
            if m := BATCH.search(line):
                starts[int(m[1])] = int(m[2])
            elif m := LANE.search(line):
                lanes[int(m[1])][m[2]] = float(m[3])
    return starts, lanes


def batch_of(tick: int, starts: dict[int, int]) -> int:
    return max((b for b, s in starts.items() if s <= tick), default=0)


def retail_events(log: str, beats: str) -> dict[str, list[Event]]:
    starts, lanes = batches(log)
    files = sorted(Path(beats).rglob("beat*.log"))
    names: dict[tuple[int, str], str] = {}  # (batch, id) -> blueprint
    out: dict[str, list[Event]] = defaultdict(list)
    for path in files:
        beat = int(path.stem[4:])
        batch = batch_of(beat, starts)
        if not batch:
            continue
        rel = beat - starts[batch]
        unit = ""
        nav = ""
        pending: tuple[str, str] | None = None
        with open(path, encoding="latin-1") as f:
            for line in f:
                if m := NAV_FOR.search(line):
                    nav = m[1]
                elif (m := CURRENT.search(line)) and nav and (batch, nav) not in names:
                    x, z = float(m[1]), float(m[2])
                    if abs(z - ZS) < 1.5:
                        best = min(lanes[batch].items(), key=lambda kv: abs(kv[1] - x))
                        if abs(best[1] - x) < 1.5:
                            names[(batch, nav)] = best[0]
                elif m := STEERING.match(line):
                    unit = m[1]
                elif m := WAYPOINTS.search(line):
                    what = f"waypoints {m[3]},{m[4]}" if m[3] else "waypoints none"
                    events = out[f"{batch}:{m[1]}"]
                    # Going idle clears the waypoints twice (NavigatorMakeIdle, then Stop).
                    if what != "waypoints none" or not events or events[-1] != (rel, what):
                        events.append((rel, what))
                elif m := RESOLVE.search(line):
                    pending = (m[1], m[2])
                    out[f"{batch}:{m[1]}"].append((rel, "resolve"))
                elif pending and "not within cone" in line:
                    out[f"{batch}:{pending[0]}"].append((rel, "hold"))
                    pending = None
                elif pending and "within cone" in line:
                    out[f"{batch}:{pending[0]}"].append((rel, "sidestep"))
                    pending = None
                elif "pushed!" in line and unit:
                    out[f"{batch}:{unit}"].append((rel, "pushed"))
    named: dict[str, list[Event]] = {}
    for key, events in out.items():
        batch, uid = key.split(":")
        if (int(batch), uid) in names:
            named[names[(int(batch), uid)]] = events
    return named


def engine_events(log: str) -> dict[str, list[Event]]:
    starts, _ = batches(log)
    out: dict[str, list[Event]] = defaultdict(list)
    with open(log, encoding="latin-1") as f:
        for line in f:
            if m := ENGINE.search(line):
                tick = int(m[1])
                batch = batch_of(tick, starts)
                if batch:
                    out[m[2]].append((tick - starts[batch], m[3]))
    return out


def kind(what: str) -> str:
    return what.split()[0]


def moves(events: list[Event]) -> list[list[tuple[int, str]]]:
    """A unit's events split into its moves, each ending as its navigator goes idle, timed from
    the move's first event."""
    out: list[list[tuple[int, str]]] = []
    current: list[tuple[int, str]] = []
    for t, what in events:
        current.append((t, what))
        if what == "waypoints none":
            out.append(current)
            current = []
    if current:
        out.append(current)
    return [[(t - m[0][0], kind(w)) for t, w in m] for m in out]


def main() -> int:
    retail = retail_events(sys.argv[1], sys.argv[2])
    engine = engine_events(sys.argv[3])
    totals_r: Counter[str] = Counter(kind(w) for ev in retail.values() for _, w in ev)
    totals_e: Counter[str] = Counter(kind(w) for ev in engine.values() for _, w in ev)
    print(f"units named: original {len(retail)}, engine {len(engine)}")
    for k in sorted(totals_r.keys() | totals_e.keys()):
        print(f"  {k:10s} original {totals_r[k]:6d}  engine {totals_e[k]:6d}")
    differ = 0
    firsts: Counter[str] = Counter()
    for uid in sorted(retail.keys() | engine.keys()):
        r_moves = moves(retail.get(uid, []))
        e_moves = moves(engine.get(uid, []))
        for i in range(max(len(r_moves), len(e_moves))):
            r = r_moves[i] if i < len(r_moves) else []
            e = e_moves[i] if i < len(e_moves) else []
            first = next((j for j, (a, b) in enumerate(zip(r, e)) if a != b), None)
            if first is None and len(r) == len(e):
                continue
            differ += 1
            j = first if first is not None else min(len(r), len(e))
            got_r = r[j] if j < len(r) else None
            got_e = e[j] if j < len(e) else None
            firsts[f"{got_r[1] if got_r else '-'} / {got_e[1] if got_e else '-'}"] += 1
            print(f"    {uid} move {i + 1} event {j}: original {got_r}, engine {got_e}")
            break
    print(f"units whose steering differs: {differ}")
    print("first differences (original / engine):")
    for k, n in firsts.most_common():
        print(f"  {n:4d}  {k}")
    return 1 if differ else 0


if __name__ == "__main__":
    sys.exit(main())
