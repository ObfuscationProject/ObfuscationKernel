#!/usr/bin/env python3
"""Hosted POSIX/ext4 truncate and I/O fault replay; no guest CPU or transport."""

import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


ARCHITECTURES = (
    "i386", "x86_64", "aarch64", "arm32", "rv64", "rv32",
    "loongarch64", "mips", "mips64", "ppc",
)
SOURCES = (
    "tests/host/posix_ext4_truncate_tests.cpp",
    "tests/host/ext4_realtime_model.cpp",
    "src/sched/scheduler.cpp", "src/sched/process.cpp",
    "src/posix/posix.cpp", "src/posix/fd.cpp", "src/posix/path.cpp",
    "src/posix/process.cpp", "src/fs/vfs.cpp", "src/fs/ext4_fs.cpp",
    "src/driver/driver.cpp", "src/smp/smp.cpp", "src/arch/arch.cpp",
    "src/syscall/linux.cpp",
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arch", choices=("all", *ARCHITECTURES), default="x86_64")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="ok-posix-ext4-truncate-") as temporary:
        output = Path(temporary)
        images = []
        for block_size in (1024, 2048, 4096):
            image = output / f"ext4-{block_size}.img"
            subprocess.run([
                "mke2fs", "-q", "-t", "ext4", "-b", str(block_size), "-I", "256",
                "-O", "none,extent,filetype,sparse_super,large_file",
                "-E", "lazy_itable_init=0", "-m", "0", str(image), "16M",
            ], check=True)
            images.append((block_size, image))
        profiles = ARCHITECTURES if args.arch == "all" else (args.arch,)
        for arch in profiles:
            executable = output / f"truncate-{arch}"
            subprocess.run([
                *shlex.split(args.cxx), "-std=c++23", "-O1" if args.sanitize else "-O2",
                "-g", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                *(["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
                  if args.sanitize else []),
                f"-DOK_ARCH_TARGET_{arch.upper()}", f"-I{root / 'include'}",
                *(str(root / source) for source in SOURCES),
                str(root / f"src/arch/{arch}/ops.cpp"), "-o", str(executable),
            ], check=True)
            for block_size, image in images:
                subprocess.run([str(executable), str(image)], check=True)
                print(f"PASS {arch}: hosted POSIX/ext4 {block_size}-byte truncate "
                      "replay and fault sweep; no guest transport", flush=True)


if __name__ == "__main__":
    main()
