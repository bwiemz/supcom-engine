#!/usr/bin/env python3
"""Fail if the engine's CMake library targets link in a cycle, or across a layer (M191).

A cycle among the osc_* targets means the targets don't mark real
boundaries. So does a layer reaching up: the sim, its Lua library or the
blueprints must not depend (even through another target) on the renderer or
on what uses them. CMake writes the targets' links when it configures
(cmake/OscLinkGraph.cmake); this walks them.

    tools/check_link_cycles.py build/linux-debug/osc_link_graph.txt
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

# (target, what it must not reach, why)
FORBIDDEN: tuple[tuple[str, str, str], ...] = (
    (
        "osc_lua",
        "osc_renderer",
        "the sim's Lua library runs headless (user bindings are osc_lua_user)",
    ),
    ("osc_sim", "osc_renderer", "the sim runs headless"),
    ("osc_sim", "osc_lua", "the sim sits below its Lua bindings"),
    ("osc_blueprints", "osc_lua", "blueprints sit below the sim and its bindings"),
)


def reaches(graph: dict[str, set[str]], src: str, dst: str) -> bool:
    seen: set[str] = set()
    todo = [src]
    while todo:
        node = todo.pop()
        for nxt in graph.get(node, ()):
            if nxt == dst:
                return True
            if nxt not in seen:
                seen.add(nxt)
                todo.append(nxt)
    return False


def read_graph(path: Path) -> dict[str, set[str]]:
    """Map each osc_* target to the osc_* targets it links."""
    graph: dict[str, set[str]] = {}
    for line in path.read_text().splitlines():
        parts = line.split()
        if len(parts) != 2:
            continue
        src, dst = parts
        if src.startswith("osc_") and dst.startswith("osc_") and src != dst:
            graph.setdefault(src, set()).add(dst)
    return graph


def find_cycle(graph: dict[str, set[str]]) -> list[str] | None:
    """A cycle as a list of targets (first == last), or None."""
    state: dict[str, int] = {}  # 1: on the path, 2: done
    path: list[str] = []

    def visit(node: str) -> list[str] | None:
        state[node] = 1
        path.append(node)
        for nxt in sorted(graph.get(node, ())):
            if state.get(nxt) == 1:
                return [*path[path.index(nxt) :], nxt]
            if nxt not in state:
                found = visit(nxt)
                if found:
                    return found
        _ = path.pop()
        state[node] = 2
        return None

    for node in sorted(graph):
        if node not in state:
            found = visit(node)
            if found:
                return found
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description="Fail if the osc_* targets link in a cycle.")
    _ = parser.add_argument("graph", help="the link graph CMake wrote (osc_link_graph.txt)")
    args = parser.parse_args()
    path = Path(str(args.graph))  # pyright: ignore[reportAny] -- argparse values are untyped
    if not path.is_file():
        print(f"{path}: no link graph (configure the build first)")
        return 2
    graph = read_graph(path)
    if not graph:
        print("no osc_* targets found in the graph")
        return 2
    failed = False
    cycle = find_cycle(graph)
    if cycle:
        print("the library targets link in a cycle: " + " -> ".join(cycle))
        failed = True
    for src, dst, why in FORBIDDEN:
        if src in graph and reaches(graph, src, dst):
            print(f"{src} depends on {dst}: {why}")
            failed = True
    if failed:
        return 1
    print(f"no cycle or layer crossing among {len(graph)} linking osc_* targets")
    return 0


if __name__ == "__main__":
    sys.exit(main())
