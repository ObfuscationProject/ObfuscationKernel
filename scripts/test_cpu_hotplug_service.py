#!/usr/bin/env python3
"""Hosted model test of the user CPU hotplug service (include/ok/smp/hotplug.hpp, smp-scaling WP1).

Model coverage only: a model backend stands in for stop_secondary_cpu, restart_secondary_cpu and the
scheduler; no CPU stops, starts or parks. Covers the record lifecycle and consume-once results, no-op
and coalesced requests, refusals, orphaned and abandoned records, the result mapping, a wedged CPU and
its recovery, service order and the kstrtobool parser. Built with OK_HOSTED_LOCK_MODEL (the record lock
is the real spin lock and host threads race writers against the serving CPU) with and without
OK_LOCK_DEBUG, and once without it (an UpLock, no race).
"""
import argparse
import os
import re
import shlex
import subprocess
import tempfile
from pathlib import Path

TEST = "tests/host/cpu_hotplug_service_tests.cpp"
MARKER = r"^CPU_HOTPLUG_SERVICE_MODEL_PASS records=128 concurrent=%d constinit=1 scope=model$"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--output", type=Path, help="keep the binaries here instead of a temporary directory")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    flags = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
             "-fno-omit-frame-pointer"] if args.sanitize else []
    with tempfile.TemporaryDirectory(prefix="ok-cpu-hotplug-") as scratch:
        directory = args.output or Path(scratch)
        directory.mkdir(parents=True, exist_ok=True)
        for name, defines, concurrent in (("debug", ["-DOK_HOSTED_LOCK_MODEL", "-DOK_LOCK_DEBUG"], 1),
                                          ("release", ["-DOK_HOSTED_LOCK_MODEL"], 1), ("up", [], 0)):
            binary = directory / f"cpu-hotplug-service-{name}"
            subprocess.run([*shlex.split(args.cxx), "-std=c++23", "-O2", "-g", "-Wall", "-Wextra", "-Wpedantic",
                            "-Werror", "-fno-exceptions", "-pthread", "-DOK_ARCH_TARGET_X86_64", *flags, *defines,
                            f"-I{root / 'include'}", str(root / TEST), str(root / "src/arch/x86_64/ops.cpp"),
                            "-o", str(binary)], check=True)
            result = subprocess.run([str(binary)], check=True, stdout=subprocess.PIPE, text=True)
            if not re.search(MARKER % concurrent, result.stdout, re.M):
                raise SystemExit(f"cpu-hotplug-service-{name}: missing exact success marker\n{result.stdout}")
            print(f"PASS cpu-hotplug-service ({name}): {result.stdout.splitlines()[0]}; model scope", flush=True)


if __name__ == "__main__":
    main()
