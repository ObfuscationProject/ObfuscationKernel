#!/usr/bin/env python3
"""Ratchet for the native runtime split (src/user/README.md).

runtime.cpp must not regrow, the units must exist, and headers under src/user
must be self-contained instead of textual fragments included mid-namespace.
"""
import re
import sys
from pathlib import Path

LIMITS = {"lines": 675, "includes": 1}
UNITS = ("loader", "vm", "wait", "futex", "pipe", "pty", "net", "signals", "entry")
ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    user = ROOT / "src/user"
    text = (user / "runtime.cpp").read_text(errors="surrogateescape")
    lines = text.count("\n")
    includes = len(re.findall(r"^\s*#include", text, re.M))
    rc = 0
    for name, value in (("lines", lines), ("includes", includes)):
        status = "ok" if value <= LIMITS[name] else "GREW"
        if status == "GREW":
            rc = 1
        print(f"runtime.cpp {name}: {value} (limit {LIMITS[name]}) {status}")
    for unit in UNITS:
        path = user / "runtime" / f"{unit}.cpp"
        if not path.is_file():
            print(f"missing unit {path.relative_to(ROOT)}")
            rc = 1
            continue
        print(f"{path.relative_to(ROOT)}: {path.read_text(errors='surrogateescape').count(chr(10))} lines")
    for header in sorted(user.rglob("*.hpp")):
        if "#pragma once" not in header.read_text(errors="surrogateescape"):
            print(f"textual fragment {header.relative_to(ROOT)}: headers must be self-contained")
            rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
