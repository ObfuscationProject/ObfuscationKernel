#!/usr/bin/env python3
"""Model the user-access window protocol with poisoned frees (smp-scaling WP3).

Host threads stand in for CPUs: kernel copies and user entries open their windows on a model
backend that publishes entries and slots like the x86_64, aarch64 and rv64 backends; the writer
publishes, bumps the TLB generation, calls the real ok::smp::quiesce_root and only then poisons what
it freed. No reader may read poison; a control that frees before quiesce_root must be caught.
Built with OK_HOSTED_LOCK_MODEL and OK_LOCK_DEBUG (--sanitize adds address and undefined
sanitizers). ThreadSanitizer is not used: it does not model the seq_cst fences of the window pair
and reports the pair as a race. Model coverage only (scope=model): no MMU, TLB, IPI or hardware
memory ordering.
"""
import argparse
import os
import shlex
import subprocess
import tempfile
from pathlib import Path

MARKER = "OK_TRANSLATION_WINDOWS_MODEL scope=model ordered=pass control=pass window_wait=pass"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    flags = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
             "-fno-omit-frame-pointer"] if args.sanitize else []
    with tempfile.TemporaryDirectory(prefix="ok-translation-windows-") as directory:
        output = Path(directory) / "translation-windows"
        subprocess.run([
            *shlex.split(args.cxx), "-std=c++23", "-O2", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
            "-fno-exceptions", "-pthread", *flags, "-DOK_HOSTED_LOCK_MODEL", "-DOK_LOCK_DEBUG",
            f"-I{root / 'include'}", str(root / "tests/host/translation_windows_tests.cpp"),
            "-o", str(output)], check=True)
        # The poisoned frames stay allocated on purpose (never reused).
        environment = dict(os.environ, ASAN_OPTIONS="detect_leaks=0:halt_on_error=1",
                           UBSAN_OPTIONS="halt_on_error=1")
        result = subprocess.run([str(output)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                env=environment)
        print(result.stdout, end="")
        if result.returncode or MARKER not in result.stdout:
            raise SystemExit(f"translation windows model failed (exit status {result.returncode})")


if __name__ == "__main__":
    main()
