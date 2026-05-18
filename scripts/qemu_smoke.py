#!/usr/bin/env python3
"""Run an ObfuscationKernel smoke binary directly or through qemu-user."""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path


QEMU_BY_ARCH = {
    "i386": "qemu-i386",
    "x86_64": "qemu-x86_64",
    "x64": "qemu-x86_64",
    "aarch64": "qemu-aarch64",
    "arm32": "qemu-arm",
    "arm": "qemu-arm",
    "rv64": "qemu-riscv64",
    "riscv64": "qemu-riscv64",
    "rv32": "qemu-riscv32",
    "riscv32": "qemu-riscv32",
    "loongarch64": "qemu-loongarch64",
}

QEMU_LD_PREFIX_BY_ARCH = {
    "i386": "/usr/i686-linux-gnu",
    "aarch64": "/usr/aarch64-linux-gnu",
    "arm32": "/usr/arm-linux-gnueabihf",
    "arm": "/usr/arm-linux-gnueabihf",
    "rv64": "/usr/riscv64-linux-gnu",
    "riscv64": "/usr/riscv64-linux-gnu",
}


def command_for(arch: str, binary: Path, direct: bool) -> list[str]:
    if direct or arch == "host":
        return [str(binary)]
    qemu = QEMU_BY_ARCH.get(arch)
    if qemu is None:
        raise SystemExit(f"unsupported qemu smoke architecture: {arch}")
    qemu_path = shutil.which(qemu)
    if qemu_path is None:
        raise SystemExit(f"{qemu} was not found in PATH")
    return [qemu_path, str(binary)]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--arch", required=True)
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--direct", action="store_true", help="Run the smoke binary directly as a hosted profile test")
    args = parser.parse_args()

    binary = args.binary.resolve()
    if not binary.exists():
        print(f"smoke binary does not exist: {binary}", file=sys.stderr)
        return 2

    command = command_for(args.arch, binary, args.direct)
    env = os.environ.copy()
    ld_prefix = QEMU_LD_PREFIX_BY_ARCH.get(args.arch)
    if "QEMU_LD_PREFIX" not in env and ld_prefix and Path(ld_prefix).exists():
        env["QEMU_LD_PREFIX"] = ld_prefix

    result = subprocess.run(command, text=True, capture_output=True, check=False, env=env)
    if result.stdout:
        print(result.stdout, end="")
    if result.stderr:
        print(result.stderr, end="", file=sys.stderr)
    if result.returncode != 0:
        return result.returncode
    if "OK_TEST_PASS" not in result.stdout:
        print("smoke binary did not report OK_TEST_PASS", file=sys.stderr)
        return 3
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
