#!/usr/bin/env python3
"""Ratchet for the native runtime split (src/user/README.md).

runtime.cpp must not regrow, the units must exist, and headers under src/user
must be self-contained instead of textual fragments included mid-namespace;
per-architecture signal pieces stay collapsed in include/ok/arch/<arch>/signal.hpp.
"""
import re
import sys
from pathlib import Path

# 675 at the split (WT1.4); WT1.2 added emit_line() and its line state (13
# lines), diagnostics output that is runtime.cpp's own responsibility. New
# responsibilities go into units: WT1.2's interval timers and interrupt report.
LIMITS = {"lines": 688, "includes": 1}
UNITS = ("loader", "vm", "wait", "futex", "pipe", "pty", "net", "signals", "timers", "interrupts", "entry")
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
    # Architecture signal code lives in include/ok/arch/<arch>/signal.hpp.
    pieces = r"native_signal_(delivery|frame|return|context|gate|readiness|persistent)_\w+\.hpp"
    for header in sorted((ROOT / "include/ok/user").glob("native_signal_*.hpp")):
        if re.fullmatch(pieces, header.name):
            print(f"per-architecture signal header {header.relative_to(ROOT)}: use include/ok/arch/<arch>/signal.hpp")
            rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
