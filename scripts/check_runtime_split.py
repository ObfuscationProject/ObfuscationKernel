#!/usr/bin/env python3
"""Ratchet for refactor/user-runtime-split: runtime.cpp must not grow.

Lower the limits here as code moves into src/user/{loader,entry,vm,wait,pty,net}.cpp.
"""
import re
import sys
from pathlib import Path

LIMITS = {"lines": 5876, "includes": 101}
ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    source = ROOT / "src/user/runtime.cpp"
    text = source.read_text(errors="surrogateescape")
    lines = text.count("\n")
    includes = len(re.findall(r"^\s*#include", text, re.M))
    rc = 0
    for name, value in (("lines", lines), ("includes", includes)):
        status = "ok" if value <= LIMITS[name] else "GREW"
        if status == "GREW":
            rc = 1
        print(f"runtime.cpp {name}: {value} (limit {LIMITS[name]}) {status}")
    units = sorted(p.name for p in (ROOT / "src/user").glob("*.cpp"))
    print("units:", ", ".join(units))
    return rc


if __name__ == "__main__":
    sys.exit(main())
