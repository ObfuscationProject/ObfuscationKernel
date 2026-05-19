#!/usr/bin/env python3
"""Run an ObfuscationKernel debug kernel test binary."""

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
    "loong64": "qemu-loongarch64",
    "mips": "qemu-mips",
    "mips64": "qemu-mips64",
    "ppc": "qemu-ppc",
    "ppc64": "qemu-ppc64",
}

QEMU_LD_PREFIX_BY_ARCH = {
    "i386": "/usr/i686-linux-gnu",
    "aarch64": "/usr/aarch64-linux-gnu",
    "arm32": "/usr/arm-linux-gnueabihf",
    "arm": "/usr/arm-linux-gnueabihf",
    "rv64": "/usr/riscv64-linux-gnu",
    "riscv64": "/usr/riscv64-linux-gnu",
}


def parse_fields(line: str) -> dict[str, str]:
    fields: dict[str, str] = {}
    for token in line.strip().split()[1:]:
        if "=" in token:
            key, value = token.split("=", 1)
            fields[key] = value
    return fields


def command_for(arch: str, binary: Path, direct: bool) -> list[str]:
    if direct:
        return [str(binary)]
    qemu = QEMU_BY_ARCH.get(arch)
    if qemu is None:
        raise SystemExit(f"unsupported qemu test architecture: {arch}")
    qemu_path = shutil.which(qemu)
    if qemu_path is None:
        raise SystemExit(f"{qemu} was not found in PATH")
    return [qemu_path, str(binary)]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--arch", required=True)
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--direct", action="store_true", help="Run the debug kernel test binary directly")
    args = parser.parse_args()

    binary = args.binary.resolve()
    if not binary.exists():
        print(f"debug kernel test binary does not exist: {binary}", file=sys.stderr)
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

    lines = result.stdout.splitlines()
    if "OK_MODE debug" not in lines:
        print("test binary did not boot a debug kernel", file=sys.stderr)
        return 3
    if not any(line.startswith("OK_DEBUG boot=complete") for line in lines):
        print("debug kernel did not report boot completion", file=sys.stderr)
        return 4

    pass_lines = [line for line in lines if line.startswith("OK_TEST_PASS ")]
    if not pass_lines:
        print("debug kernel did not report OK_TEST_PASS", file=sys.stderr)
        return 5

    fields = parse_fields(pass_lines[-1])
    expected_arch = args.arch
    if fields.get("arch") != expected_arch:
        print(f"debug kernel arch mismatch: expected {expected_arch}, got {fields.get('arch')}", file=sys.stderr)
        return 6
    if int(fields.get("debug_test_points", "0")) == 0:
        print("debug kernel did not run debug test points", file=sys.stderr)
        return 7
    for required in ("fs", "ext4", "user", "display"):
        if fields.get(required) != "1":
            print(f"debug kernel did not pass {required} test coverage", file=sys.stderr)
            return 8
    if int(fields.get("display_checksum", "0")) == 0:
        print("display driver did not produce a framebuffer checksum", file=sys.stderr)
        return 9
    if not any(line.startswith("OK_DISPLAY_TEXT ") for line in lines):
        print("display driver did not report boot text", file=sys.stderr)
        return 10
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
