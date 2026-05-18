#!/usr/bin/env python3
"""Build every architecture profile and run direct profile smoke tests."""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path


ARCHES = ("host", "i386", "x86_64", "aarch64", "arm32", "rv64", "rv32", "loongarch64")


def run(command: list[str], cwd: Path) -> None:
    print("+ " + " ".join(command), flush=True)
    subprocess.run(command, cwd=cwd, check=True)


def newest_smoke_binary(root: Path, mode: str) -> Path:
    candidates = [path for path in root.glob(f"build/**/{mode}/qemu_smoke") if path.is_file()]
    if not candidates:
        raise FileNotFoundError(f"qemu_smoke binary was not produced for mode {mode}")
    return max(candidates, key=lambda path: path.stat().st_mtime)


def run_direct_smoke(binary: Path, arch: str) -> None:
    result = subprocess.run([str(binary)], text=True, capture_output=True, check=False)
    if result.stdout:
        print(result.stdout, end="")
    if result.stderr:
        print(result.stderr, end="", file=sys.stderr)
    if result.returncode != 0:
        raise subprocess.CalledProcessError(result.returncode, [str(binary)])
    if f"arch={arch}" not in result.stdout:
        raise RuntimeError(f"smoke output did not contain arch={arch}")
    if "OK_TEST_PASS" not in result.stdout:
        raise RuntimeError("smoke output did not contain OK_TEST_PASS")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", choices=("debug", "release"), default="debug")
    parser.add_argument("--no-run", action="store_true", help="Only build each profile")
    parser.add_argument("--no-restore-host", action="store_true", help="Leave xmake configured for the last checked arch")
    args = parser.parse_args()

    root = Path(__file__).resolve().parents[1]
    for arch in ARCHES:
        run(["xmake", "f", "-c", "-m", args.mode, f"--arch_target={arch}"], root)
        run(["xmake", "-y", "-b", "qemu_smoke"], root)
        if not args.no_run:
            run_direct_smoke(newest_smoke_binary(root, args.mode), arch)

    if not args.no_restore_host:
        run(["xmake", "f", "-c", "--arch_target=host"], root)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
