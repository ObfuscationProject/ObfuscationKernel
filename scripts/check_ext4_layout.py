#!/usr/bin/env python3
"""Check the ext4 module layout: the eight module headers under include/ok/fs/ext4/
include only modules below them, every private partial under ext4/detail/ has one
includer inside the module set, no per-slice header or old include path remains
besides the two SDK contract markers, and every module compiles standalone on the
host."""
import os
import re
import shlex
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FS = ROOT / "include/ok/fs"
EXT4 = FS / "ext4"
# Bottom to top; a module may include only modules listed before it.
LAYERS = ("journal", "layout", "extent", "alloc", "dir", "txn", "recovery", "mount")
INCLUDE = re.compile(r'^\s*#include [<"]ok/fs/([^">]+)[">]', re.M)
# Small unconditional marker headers that tools/export_sdk.py and the OS consumer parse;
# they stay outside the modules.
SDK_MARKERS = ("ext4_directory_create_sdk_profile.hpp", "ext4_mounted_common_profile.hpp")
OLD_PATH = re.compile(r'ok/fs/(?:ext4(?:_[a-z0-9_]+)?|jbd2(?:_replay)?|metadata_buffers)\.(?:hpp|inc)\b')
SOURCES = ("src", "tests", "tools", "include")


def main() -> int:
    rc = 0

    def fail(message: str) -> None:
        nonlocal rc
        print(message)
        rc = 1

    stale = sorted(p.name for p in FS.iterdir()
                   if p.is_file() and p.name not in SDK_MARKERS and OLD_PATH.search("ok/fs/" + p.name))
    for name in stale:
        fail(f"per-slice header remains: include/ok/fs/{name}")
    modules = sorted(p.stem for p in EXT4.glob("*.hpp"))
    if modules != sorted(LAYERS):
        fail(f"module set differs: {modules}")
    rank = {name: index for index, name in enumerate(LAYERS)}
    partials = {p.name: [] for p in (EXT4 / "detail").glob("*.inc")}
    for path in [*(EXT4 / f"{m}.hpp" for m in modules), *sorted((EXT4 / "detail").glob("*.inc"))]:
        for name in INCLUDE.findall(path.read_text()):
            if name.startswith("ext4/detail/"):
                partial = name[len("ext4/detail/"):]
                if partial not in partials:
                    fail(f"{path.relative_to(ROOT)}: includes missing partial {name}")
                else:
                    partials[partial].append(path.name)
            elif name.startswith("ext4/"):
                if path.suffix == ".inc":
                    fail(f"{path.relative_to(ROOT)}: partial includes module {name}")
                    continue
                lower = Path(name).stem
                if lower not in rank or rank[lower] >= rank[path.stem]:
                    fail(f"{path.name}: includes {name}, which is not a lower layer")
    for partial, owners in sorted(partials.items()):
        if len(owners) != 1:
            fail(f"ext4/detail/{partial}: included by {owners or 'nobody'}")
    for base in SOURCES:
        for path in sorted((ROOT / base).rglob("*")):
            if not path.is_file() or path.suffix not in (".cpp", ".hpp", ".h", ".inc", ".py", ".lua"):
                continue
            if path.is_relative_to(EXT4) or path == Path(__file__).resolve():
                continue
            text = path.read_text(errors="replace")
            for match in OLD_PATH.finditer(text):
                if match.group(0).rsplit("/", 1)[1] not in SDK_MARKERS:
                    fail(f"{path.relative_to(ROOT)}: old ext4 path {match.group(0)}")
            if "ext4/detail/" in text and path.suffix != ".py":
                fail(f"{path.relative_to(ROOT)}: includes a private ext4 partial")
    # Self-containment: each module compiles on its own.
    cxx = shlex.split(os.environ.get("CXX", "g++"))
    for name in modules:
        result = subprocess.run([*cxx, "-std=c++23", "-fsyntax-only", "-fno-exceptions", "-Wall", "-Wextra",
                                 "-Wpedantic", "-Werror", f"-I{ROOT / 'include'}", "-x", "c++", "-"],
                                input=f'#include "ok/fs/ext4/{name}.hpp"\n', capture_output=True, text=True)
        if result.returncode != 0:
            fail(f"ext4/{name}.hpp is not self-contained:\n{result.stderr[:2000]}")
    print(f"ext4 layout: {len(modules)} modules, {len(partials)} partials, {'ok' if rc == 0 else 'FAILED'}")
    return rc


if __name__ == "__main__":
    sys.exit(main())
