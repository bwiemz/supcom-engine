#!/usr/bin/env python3
"""Check that the places a release's version is written agree (M227a).

The version is CMake's `project(VERSION)`. vcpkg.json repeats it, and
CHANGELOG.md names each release: a release bumps them together.

  - vcpkg.json's "version-string" is CMake's version;
  - no CHANGELOG.md release heading (`## [x.y.z]`) is newer than it (notes
    for a release the build doesn't claim to be);
  - with --tag vX.Y.Z (the release workflow): the tag is the version, and
    CHANGELOG.md has its heading.

Usage: check_version.py <source dir> [--tag vX.Y.Z]   (exit 1 on a mismatch)
       check_version.py --self-test
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path
from typing import cast

_PROJECT = re.compile(r"project\(\s*\w+\s+VERSION\s+(\d+\.\d+\.\d+)", re.IGNORECASE)
_RELEASE = re.compile(r"^## \[(\d+\.\d+\.\d+)\]", re.MULTILINE)

Version = tuple[int, int, int]


def parse(text: str) -> Version:
    major, minor, patch = (int(p) for p in text.split("."))
    return (major, minor, patch)


def cmake_version(cmakelists: str) -> str | None:
    m = _PROJECT.search(cmakelists)
    return m.group(1) if m else None


def changelog_releases(changelog: str) -> list[str]:
    return _RELEASE.findall(changelog)


def problems(cmakelists: str, vcpkg_json: str, changelog: str, tag: str | None) -> list[str]:
    version = cmake_version(cmakelists)
    if version is None:
        return ["CMakeLists.txt: no project(... VERSION x.y.z)"]
    found: list[str] = []
    manifest = cast("dict[str, object]", json.loads(vcpkg_json))
    vcpkg = manifest.get("version-string")
    if vcpkg != version:
        found.append(f"vcpkg.json version-string {vcpkg!r} is not CMake's {version}")
    releases = changelog_releases(changelog)
    for release in releases:
        if parse(release) > parse(version):
            found.append(f"CHANGELOG.md has release {release}, newer than CMake's {version}")
    if tag is not None:
        if tag != f"v{version}":
            found.append(f"tag {tag} is not v{version}")
        if version not in releases:
            found.append(f"CHANGELOG.md has no '## [{version}]' section for the release")
    return found


def self_test() -> int:
    cm = "cmake_minimum_required(VERSION 3.21)\nproject(OpenSupCom VERSION 0.2.0 LANGUAGES C CXX)\n"
    vj = '{"name": "opensupcom", "version-string": "0.2.0"}'
    log = "# Changelog\n\n## [Unreleased]\n\n## [0.2.0] - 2026-10-01\n\n## [0.1.0] - 2026-09-30\n"
    cases = [
        (problems(cm, vj, log, None), 0),
        (problems(cm, vj, log, "v0.2.0"), 0),
        (problems(cm, vj.replace("0.2.0", "0.1.0"), log, None), 1),  # vcpkg behind
        (
            problems(cm.replace("0.2.0", "0.1.0"), vj.replace("0.2.0", "0.1.0"), log, None),
            1,
        ),  # notes ahead
        (problems(cm, vj, log, "v0.1.0"), 1),  # the wrong tag
        (problems(cm, vj, "# Changelog\n\n## [Unreleased]\n", "v0.2.0"), 1),  # no notes
        (problems("project(X)", vj, log, None), 1),  # no version
        # 0.10.0 is newer than 0.9.0: compared as numbers, not text
        (
            problems(
                cm.replace("0.2.0", "0.10.0"),
                vj.replace("0.2.0", "0.10.0"),
                "## [0.9.0] - x\n",
                None,
            ),
            0,
        ),
    ]
    failed = [i for i, (found, want) in enumerate(cases) if len(found) != want]
    for i in failed:
        print(f"self-test case {i}: {cases[i][0]}")
    print("self-test:", "FAIL" if failed else "ok")
    return 1 if failed else 0


def main(argv: list[str]) -> int:
    if argv[1:] == ["--self-test"]:
        return self_test()
    if len(argv) not in (2, 4) or (len(argv) == 4 and argv[2] != "--tag"):
        print(__doc__)
        return 2
    root = Path(argv[1])
    tag = argv[3] if len(argv) == 4 else None
    found = problems(
        (root / "CMakeLists.txt").read_text(encoding="utf-8"),
        (root / "vcpkg.json").read_text(encoding="utf-8"),
        (root / "CHANGELOG.md").read_text(encoding="utf-8"),
        tag,
    )
    for p in found:
        print(p)
    if not found:
        print(f"version {cmake_version((root / 'CMakeLists.txt').read_text(encoding='utf-8'))}: ok")
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
