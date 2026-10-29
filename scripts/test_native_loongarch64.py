#!/usr/bin/env python3
"""Build and run a real LoongArch PLV3/TLB/timer probe without xmake changes."""
from __future__ import annotations
import argparse
from pathlib import Path
import subprocess


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--toolchain", type=Path)
    parser.add_argument("--qemu", default="qemu-system-loongarch64")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--nx-branch", action="store_true",
                        help="Require actual NX branch delivery; QEMU 8.2.2 recursively faults before EENTRY")
    parser.add_argument("--kernel-fault", action="store_true",
                        help="Require safe PLV0 fault handling after the normal user-mode checks")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    toolchain = args.toolchain or root / "toolchains/loongarch64-elf"
    compiler = toolchain.resolve() / "bin/loongarch64-elf-g++"
    output = (args.output or root / "build/native-tests/loongarch64").resolve()
    output.mkdir(parents=True, exist_ok=True)
    flags = ["-msimd=none"]
    cxx = ["-std=c++23", "-O2", "-ffreestanding", "-fno-builtin", "-fno-exceptions",
           "-fno-stack-protector", "-fno-threadsafe-statics", "-Wall", "-Wextra", "-Wpedantic",
           "-Werror", "-I", str(root / "include")]
    if args.nx_branch:
        cxx.append("-DOK_NATIVE_NX_BRANCH=1")
    if args.kernel_fault:
        cxx.append("-DOK_NATIVE_KERNEL_FAULT=1")
    sources = ["src/boot/raw_handoff.cpp", "src/arch/loongarch64/boot.S", "src/arch/loongarch64/execution.cpp", "src/interrupt/platform_irq.cpp",
               "src/arch/loongarch64/execution.S", "tests/native/loongarch64_execution_test.cpp",
               "tests/native/loongarch64_execution_user.S"]
    objects = []
    for index, source in enumerate(sources):
        obj = output / f"{index}.o"
        subprocess.run([str(compiler), *flags, *(cxx if source.endswith(".cpp") else []),
                        "-c", str(root / source), "-o", str(obj)], check=True)
        objects.append(str(obj))
    elf = output / "probe.elf"
    subprocess.run([str(compiler), *flags, "-nostdlib", "-static", "-Wl,--build-id=none",
                    "-Wl,-T," + str(root / "src/arch/loongarch64/linker.ld"), *objects,
                    "-lgcc", "-o", str(elf)], check=True)
    command = [args.qemu, "-M", "virt", "-m", "2G", "-kernel", str(elf), "-display", "none",
               "-serial", "stdio", "-monitor", "none", "-no-reboot"]
    try:
        result = subprocess.run(command, capture_output=True, timeout=5)
        serial = result.stdout + result.stderr
    except subprocess.TimeoutExpired as error:
        # The probe halts after emitting its final result; subprocess terminates
        # QEMU at the timeout and preserves the complete serial output.
        serial = (error.stdout or b"") + (error.stderr or b"")
    text = serial.decode(errors="replace")
    (output / "serial.log").write_text(text)
    print(text, end="")
    expected = "OK_NATIVE_KERNEL_FAULT loongarch64" if args.kernel_fault else "LOONG_NATIVE_PASS scope=hardware"
    if expected not in text or "LOONG_NATIVE_FAIL" in text:
        raise RuntimeError("LoongArch native hardware probe failed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
