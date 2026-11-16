#!/usr/bin/env python3
"""Hosted model test for CPU topology discovery (feat/smp-bringup).

Synthetic FDT blobs and synthetic ACPI physical memory drive parse_fdt_cpus,
find_acpi_rsdp and parse_acpi_madt. With --qemu-dir (repeatable, any directory
holding qemu-system-aarch64 or qemu-system-riscv64) the parser also reads DTBs
that QEMU dumps for its virt machines; without it those cases are skipped.

Model coverage only: parsing is not CPU startup, no secondary CPU executes.
"""
import argparse
import os
import shlex
import subprocess
import tempfile
from pathlib import Path

# name, QEMU binary, machine options, extra arguments, flavor, CPUs, expected method
QEMU_CASES = [
    ('aarch64-smp4', 'qemu-system-aarch64', 'virt', ['-cpu', 'cortex-a57', '-smp', '4'], 'arm', 4, 'psci_hvc'),
    ('aarch64-el2-smp4', 'qemu-system-aarch64', 'virt,virtualization=on', ['-cpu', 'cortex-a57', '-smp', '4'],
     'arm', 4, 'psci_smc'),
    ('aarch64-smp1', 'qemu-system-aarch64', 'virt', ['-cpu', 'cortex-a57', '-smp', '1'], 'arm', 1, 'none'),
    ('rv64-smp4', 'qemu-system-riscv64', 'virt', ['-smp', '4', '-bios', 'none'], 'riscv', 4, 'riscv_msip'),
]


def find_qemu(directories, name):
    for directory in directories:
        candidate = directory / name
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--output", type=Path, help="Directory for the binary and dumped DTBs (default: temporary)")
    parser.add_argument("--qemu-dir", type=Path, action="append", default=[],
                        help="Directory holding qemu-system-aarch64 and/or qemu-system-riscv64 (repeatable)")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="ok-cpu-topology-") as directory:
        work = args.output.resolve() if args.output else Path(directory)
        work.mkdir(parents=True, exist_ok=True)
        output = work / "cpu-topology"
        # A sanitizer report must fail the run, not just print.
        flags = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                 "-fno-omit-frame-pointer"] if args.sanitize else []
        subprocess.run([
            *shlex.split(args.cxx), "-std=c++23", "-O2", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
            "-fno-exceptions", *flags, f"-I{root / 'include'}",
            str(root / "tests/host/cpu_topology_tests.cpp"), str(root / "src/boot/fdt.cpp"),
            str(root / "src/boot/acpi_madt.cpp"), "-o", str(output)], check=True)

        cases, skipped = [], []
        directories = [path.resolve() for path in args.qemu_dir]
        for name, binary, machine, extra, flavor, cpus, method in QEMU_CASES:
            qemu = find_qemu(directories, binary)
            if qemu is None:
                skipped.append(name)
                continue
            dtb = work / f"{name}.dtb"
            dtb.unlink(missing_ok=True)
            subprocess.run([str(qemu), "-M", f"{machine},dumpdtb={dtb}", *extra, "-display", "none"],
                           check=True, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT, timeout=60)
            if not dtb.is_file():
                raise SystemExit(f"{qemu} did not dump {dtb}")
            cases += [flavor, str(dtb), str(cpus), method]
        result = subprocess.run([str(output), *cases], check=True, stdout=subprocess.PIPE, text=True)
        print(result.stdout, end="")
        counts = [token for token in result.stdout.split() if token.split("=")[0] in
                  ("fdt_cases", "acpi_cases", "fdt_prefixes", "corpus")]
        for name in skipped:
            print(f"SKIP qemu-dtb {name}: no qemu binary in --qemu-dir")
        print(f"OK_CPU_TOPOLOGY scope=model {' '.join(counts)} qemu_dtbs={len(cases) // 4} "
              f"qemu_skipped={len(skipped)}")


if __name__ == "__main__":
    main()
