#!/usr/bin/env python3
"""Build freestanding okernel for every architecture toolchain."""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path


SPECS = (
    ("i386", "ok-i386-elf", "i386-elf"),
    ("x86_64", "ok-x86_64-elf", "x86_64-elf"),
    ("aarch64", "ok-aarch64-elf", "aarch64-elf"),
    ("arm32", "ok-arm32-elf", "arm-none-eabi"),
    ("rv64", "ok-rv64-elf", "riscv64-elf"),
    ("rv32", "ok-rv32-elf", "riscv32-elf"),
    ("loongarch64", "ok-loongarch64-elf", "loongarch64-elf"),
)


def run(command: list[str], cwd: Path) -> None:
    print("+ " + " ".join(command), flush=True)
    subprocess.run(command, cwd=cwd, check=True)


def tool_path(root: Path, triple: str, tool: str) -> Path:
    return root / "toolchains" / triple / "bin" / f"{triple}-{tool}"


def check_unresolved(root: Path, triple: str) -> None:
    lib = root / "build" / "linux" / "x86_64" / "release" / "libokernel.a"
    merged = Path("/tmp") / f"okernel-{triple}.o"
    ld = tool_path(root, triple, "ld")
    nm = tool_path(root, triple, "nm")
    run([str(ld), "-r", "--whole-archive", str(lib), "-o", str(merged)], root)
    result = subprocess.run([str(nm), "-u", str(merged)], text=True, capture_output=True, check=False)
    if result.returncode != 0:
        print(result.stdout, end="")
        print(result.stderr, end="", file=sys.stderr)
        raise subprocess.CalledProcessError(result.returncode, [str(nm), "-u", str(merged)])
    unresolved = [line for line in result.stdout.splitlines() if line.strip()]
    if unresolved:
        print("\n".join(unresolved), file=sys.stderr)
        raise RuntimeError(f"{triple} has unresolved runtime symbols")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--allow-missing", action="store_true", help="Skip architectures whose toolchain is absent")
    args = parser.parse_args()

    root = Path(__file__).resolve().parents[1]
    missing: list[str] = []

    for arch, toolchain, triple in SPECS:
        compiler = tool_path(root, triple, "g++")
        if not compiler.exists() and shutil.which(f"{triple}-g++") is None:
            if args.allow_missing:
                missing.append(arch)
                print(f"[skip] {arch}: missing {triple}-g++")
                continue
            raise FileNotFoundError(f"missing toolchain for {arch}: {compiler}")
        run(["xmake", "f", "-c", "-m", "release", f"--arch_target={arch}", f"--toolchain={toolchain}"], root)
        run(["xmake", "-y", "-b", "okernel"], root)
        if compiler.exists():
            check_unresolved(root, triple)

    run(["xmake", "f", "-c", "--arch_target=host"], root)
    if missing:
        print("[missing] " + ", ".join(missing))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

