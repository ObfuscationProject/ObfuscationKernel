#!/usr/bin/env python3
"""Hosted model tests of the ext4 operation gate across CPUs.

Model coverage only: host threads stand in for CPUs through ScopedHostedCpu over
file-backed ext4 images made by mke2fs; no guest CPU, interrupt or device
transport executes. Runs the gate concurrency test and the VFS/ext4 callback
lock control and release integration tests, each built with OK_LOCK_DEBUG and
again without it (--sanitize adds ASan/UBSan, --tsan ThreadSanitizer instead).
"""
import argparse
import os
import shlex
import subprocess
import tempfile
from pathlib import Path

TESTS = (
    ("ext4-gate-concurrency", "tests/host/ext4_gate_concurrency_tests.cpp"),
    ("vfs-ext4-callback-lock-control", "tests/host/vfs_ext4_callback_lock_control.cpp"),
    ("vfs-ext4-release-integration", "tests/host/vfs_ext4_release_integration_tests.cpp"),
)
SOURCES = ("src/fs/ext4_fs.cpp", "src/fs/vfs.cpp", "tests/host/ext4_realtime_model.cpp")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--tsan", action="store_true")
    parser.add_argument("--output", type=Path, help="keep the binaries and images here instead of a temporary directory")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    if args.tsan:
        flags = ["-fsanitize=thread", "-fno-omit-frame-pointer"]
    elif args.sanitize:
        flags = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
    else:
        flags = []
    with tempfile.TemporaryDirectory(prefix="ok-ext4-gate-") as scratch:
        directory = args.output or Path(scratch)
        directory.mkdir(parents=True, exist_ok=True)
        images = []
        for block_size in (1024, 4096):
            image = directory / f"ext4-{block_size}.img"
            image.unlink(missing_ok=True)
            subprocess.run(["mke2fs", "-q", "-t", "ext4", "-b", str(block_size), "-I", "256",
                            "-O", "none,extent,filetype,sparse_super,large_file",
                            "-E", "lazy_itable_init=0", "-m", "0", str(image), "16M"], check=True)
            images.append(image)
        for label, defines in (("debug", ["-DOK_LOCK_DEBUG"]), ("release", [])):
            for name, test in TESTS:
                binary = directory / f"{name}-{label}"
                subprocess.run([
                    *shlex.split(args.cxx), "-std=c++23", "-O2", "-g", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                    "-fno-exceptions", "-pthread", "-DOK_ARCH_TARGET_X86_64", *flags, *defines,
                    f"-I{root / 'include'}", str(root / test), *(str(root / source) for source in SOURCES),
                    "-o", str(binary)], check=True)
                # ThreadSanitizer needs a fixed address layout.
                launcher = ["setarch", "-R"] if args.tsan else []
                for image in images:
                    subprocess.run([*launcher, str(binary), str(image)], check=True,
                                   env=dict(os.environ, TSAN_OPTIONS="halt_on_error=1") if args.tsan else None)
                print(f"PASS {name} ({label}): {len(images)} images; model scope, hosted CPUs", flush=True)


if __name__ == "__main__":
    main()
