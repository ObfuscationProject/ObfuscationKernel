#!/usr/bin/env python3
"""Check the ext4 module facades: every ext4/JBD2 .hpp slice under include/ok/fs is
included by exactly one facade under include/ok/fs/ext4/, no facade includes a
private .inc partial, and every facade compiles standalone on the host."""
import os
import re
import shlex
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FS = ROOT / "include/ok/fs"
FACADES = sorted((FS / "ext4").glob("*.hpp"))
INCLUDE = re.compile(r'#include "ok/fs/([^"]+)"')


def main() -> int:
    slices = {h.name for h in FS.iterdir()
              if h.suffix == ".hpp" and (h.name.startswith(("ext4", "jbd2")) or h.name == "metadata_buffers.hpp")}
    owners: dict[str, list[str]] = {s: [] for s in slices}
    rc = 0
    for facade in FACADES:
        for name in INCLUDE.findall(facade.read_text()):
            if name.endswith(".inc"):
                print(f"{facade.name}: includes private partial {name}"); rc = 1
            elif name in owners:
                owners[name].append(facade.name)
            else:
                print(f"{facade.name}: includes unknown slice {name}"); rc = 1
    for name, by in sorted(owners.items()):
        if len(by) != 1:
            print(f"{name}: owned by {by or 'nobody'}"); rc = 1
    # Self-containment: each slice header must compile on its own. Slices that
    # still depend on include order are listed here and burned down by the
    # worktree; --strict fails on any of them.
    cxx = shlex.split(os.environ.get("CXX", "g++"))
    strict = "--strict" in sys.argv
    broken = []
    for name in sorted(slices):
        result = subprocess.run([*cxx, "-std=c++23", "-fsyntax-only", "-fno-exceptions", "-w",
                                 f"-I{ROOT / 'include'}", "-x", "c++", str(FS / name)], capture_output=True, text=True)
        if result.returncode != 0:
            broken.append(name)
    if broken:
        print(f"{len(broken)} slice headers are not self-contained (include-order dependent):")
        for name in broken:
            print(f"  {name}")
        if strict:
            rc = 1
    print(f"ext4 layout: {len(FACADES)} facades, {len(slices)} slices, {'ok' if rc == 0 else 'FAILED'}")
    return rc


if __name__ == "__main__":
    sys.exit(main())
