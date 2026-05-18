#!/usr/bin/env python3
"""Build and run hosted smoke tests without inheriting freestanding toolchains."""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path


ARCHES = ("host", "i386", "x86_64", "aarch64", "arm32", "rv64", "rv32", "loongarch64")

LINUX_TOOLCHAINS = {
    "i386": ("ok-i386-linux", "i686-linux-gnu-g++"),
    "aarch64": ("ok-aarch64-linux", "aarch64-linux-gnu-g++"),
    "arm32": ("ok-arm32-linux", "arm-linux-gnueabihf-g++"),
    "rv64": ("ok-rv64-linux", "riscv64-linux-gnu-g++"),
}


def run(command: list[str], cwd: Path) -> None:
    print("+ " + " ".join(command), flush=True)
    subprocess.run(command, cwd=cwd, check=True)


def newest_smoke_binary(root: Path, mode: str) -> Path:
    candidates = [path for path in root.glob(f"build/**/{mode}/qemu_smoke") if path.is_file()]
    if not candidates:
        raise FileNotFoundError(f"qemu_smoke binary was not produced for mode {mode}")
    return max(candidates, key=lambda path: path.stat().st_mtime)


def configure(root: Path, arch: str, mode: str, toolchain: str | None) -> None:
    command = ["xmake", "f", "-c", "-m", mode, f"--arch_target={arch}"]
    if toolchain:
        command.append(f"--toolchain={toolchain}")
    run(command, root)


def smoke(root: Path, arch: str, mode: str, use_qemu_user: bool) -> None:
    toolchain = None
    if use_qemu_user:
        linux = LINUX_TOOLCHAINS.get(arch)
        if linux is None:
            raise RuntimeError(f"{arch} does not have a declared qemu-user smoke toolchain")
        toolchain, compiler = linux
        if shutil.which(compiler) is None:
            raise FileNotFoundError(f"{compiler} was not found in PATH")

    configure(root, arch, mode, toolchain)
    run(["xmake", "-y", "-b", "qemu_smoke"], root)

    binary = newest_smoke_binary(root, mode)
    command = [
        "python3",
        str(root / "scripts" / "qemu_smoke.py"),
        "--arch",
        arch,
        "--binary",
        str(binary),
    ]
    if not use_qemu_user:
        command.append("--direct")
    run(command, root)


def restore(root: Path, arch: str | None, toolchain: str | None, mode: str) -> None:
    if not arch:
        return
    command = ["xmake", "f", "-c", "-m", mode, f"--arch_target={arch}"]
    if toolchain:
        command.append(f"--toolchain={toolchain}")
    run(command, root)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--arch", default="host", help="Architecture profile to test, or all")
    parser.add_argument("--mode", choices=("debug", "release"), default="release")
    parser.add_argument("--user", action="store_true", help="Use qemu-user and Linux cross toolchains when supported")
    parser.add_argument("--restore-arch")
    parser.add_argument("--restore-toolchain")
    args = parser.parse_args()

    root = Path(__file__).resolve().parents[1]
    arches = ARCHES if args.arch == "all" else (args.arch,)

    try:
        for arch in arches:
            if arch not in ARCHES:
                raise RuntimeError(f"unsupported architecture profile: {arch}")
            use_qemu_user = args.user and arch in LINUX_TOOLCHAINS
            smoke(root, arch, args.mode, use_qemu_user)
    finally:
        restore_toolchain = args.restore_toolchain or None
        try:
            restore(root, args.restore_arch, restore_toolchain, args.mode)
        except Exception as exc:  # pragma: no cover - best-effort developer convenience
            print(f"warning: failed to restore xmake configuration: {exc}", file=sys.stderr)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
