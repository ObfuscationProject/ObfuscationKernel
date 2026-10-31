#!/usr/bin/env python3
"""Build/run actual Malta MIPS32/MIPS64 user-mode/TLB/CP0 timer probes."""
from __future__ import annotations
import argparse
from pathlib import Path
import subprocess


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arch", choices=("mips", "mips64"), default="mips")
    parser.add_argument("--toolchain", type=Path)
    parser.add_argument("--qemu")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--kernel-fault", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    compiler = (args.toolchain or root / "toolchains" / (args.arch + "-elf")) / "bin" / (args.arch + "-elf-g++")
    output = (args.output or root / "build/native-tests" / args.arch).resolve()
    output.mkdir(parents=True, exist_ok=True)
    flags = ["-march=mips32r2", "-G0"] if args.arch == "mips" else ["-march=mips64r2", "-mabi=64", "-G0"]
    cxx = ["-std=c++23", "-O2", "-ffreestanding", "-fno-builtin", "-fno-exceptions",
           "-fno-stack-protector", "-fno-threadsafe-statics", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
           "-I", str(root / "include"), "-DOK_ARCH_TARGET_" + args.arch.upper()]
    if args.kernel_fault:
        cxx.append("-DOK_NATIVE_KERNEL_FAULT=1")
    sources = ["src/boot/raw_handoff.cpp", f"src/arch/{args.arch}/boot.S", f"src/arch/{args.arch}/execution.cpp", "src/interrupt/platform_irq.cpp",
               "src/arch/clock.cpp", f"src/arch/{args.arch}/execution.S", "tests/native/mips_execution_test.cpp", "tests/native/mips_execution_user.S"]
    objects = []
    for index, source in enumerate(sources):
        obj = output / f"{index}.o"
        subprocess.run([str(compiler), *flags, *(cxx if source.endswith(".cpp") else []),
                        "-c", str(root / source), "-o", str(obj)], check=True)
        objects.append(str(obj))
    elf = output / "probe.elf"
    subprocess.run([str(compiler), *flags, "-nostdlib", "-static", "-Wl,--build-id=none",
                    "-Wl,-T," + str(root / f"src/arch/{args.arch}/linker.ld"), *objects, "-lgcc", "-o", str(elf)], check=True)
    command = [args.qemu or "qemu-system-" + args.arch, "-M", "malta", "-m", "256M", "-kernel", str(elf),
               "-display", "none", "-serial", "stdio", "-monitor", "none", "-no-reboot"]
    if args.arch == "mips64":
        command += ["-cpu", "MIPS64R2-generic"]
    try:
        result = subprocess.run(command, capture_output=True, timeout=5)
        serial = result.stdout + result.stderr
    except subprocess.TimeoutExpired as error:
        serial = (error.stdout or b"") + (error.stderr or b"")
    text = serial.decode(errors="replace")
    (output / "serial.log").write_text(text)
    print(text, end="")
    expected = "OK_NATIVE_KERNEL_FAULT mips" if args.kernel_fault else "MIPS_NATIVE_PASS scope=hardware"
    if expected not in text or "MIPS_NATIVE_FAIL" in text:
        raise RuntimeError(f"{args.arch} native hardware probe failed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
