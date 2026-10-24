#!/usr/bin/env python3
"""Test Linux static-ELF metadata/auxv; optionally audit explicit ELF paths."""
import argparse
import os
from pathlib import Path
import shlex
import struct
import subprocess
import tempfile


PROFILES = (
    ("i386", "i386-elf", 1, 1, 3), ("x86_64", "x86_64-elf", 2, 1, 62),
    ("aarch64", "aarch64-elf", 2, 1, 183), ("arm32", "arm-none-eabi", 1, 1, 40),
    ("rv64", "riscv64-elf", 2, 1, 243), ("rv32", "riscv32-elf", 1, 1, 243),
    ("loongarch64", "loongarch64-elf", 2, 1, 258), ("mips", "mips-elf", 1, 2, 8),
    ("mips64", "mips64-elf", 2, 2, 8), ("ppc", "powerpc-eabi", 1, 2, 20),
)
TARGET_FLAGS = {
    "i386": ["-mno-sse", "-mno-sse2"], "x86_64": ["-mno-red-zone"],
    "aarch64": ["-mno-outline-atomics", "-mgeneral-regs-only"], "arm32": ["-march=armv7-a", "-marm"],
    "rv64": ["-mcmodel=medany"], "rv32": ["-mcmodel=medany"], "loongarch64": [],
    "mips": ["-march=mips32r2", "-G0"], "mips64": ["-march=mips64r2", "-mabi=64", "-G0"],
    "ppc": ["-msoft-float", "-msdata=none"],
}


def cross_compile(root, toolchains, work):
    for name, triple, elf_class, endian, machine in PROFILES:
        compiler = toolchains / triple / "bin" / (triple + "-g++")
        if not compiler.is_file():
            raise ValueError("explicit target compiler unavailable: " + str(compiler))
        output = work / (name + ".o")
        subprocess.run([
            str(compiler), "-std=c++23", "-O2", "-Wall", "-Wextra", "-Werror", "-ffreestanding",
            "-fno-exceptions", "-fno-threadsafe-statics", *TARGET_FLAGS[name],
            "-DOK_ARCH_TARGET_" + name.upper(), "-DOK_EXEC_TEST_PROFILE=" + name,
            f"-I{root / 'include'}", "-c", str(root / "tests/host/linux_exec_compile.cpp"), "-o", str(output),
        ], check=True)
        data = output.read_bytes()
        order = "<" if endian == 1 else ">"
        if (data[:4] != b"\x7fELF" or data[4:6] != bytes((elf_class, endian)) or
                struct.unpack_from(order + "H", data, 18)[0] != machine):
            raise ValueError("wrong isolated object ABI: " + str(output))
        print(f"PASS isolated freestanding Linux exec compile {name} (not executed)", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--cross", action="store_true")
    parser.add_argument("--toolchain-root", type=Path)
    parser.add_argument("--elf", nargs=2, metavar=("PROFILE", "PATH"), action="append", default=[])
    parser.add_argument("--reject-elf", nargs=2, metavar=("PROFILE", "PATH"), action="append", default=[])
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    probes = []
    for expected, paths in [("accept", args.elf), ("reject", args.reject_elf)]:
        for profile, path in paths:
            probes.extend([expected, profile, str(Path(path).resolve())])
    with tempfile.TemporaryDirectory(prefix="ok-linux-exec-") as directory:
        output = Path(directory) / "linux-exec"
        # Only ElfLoader sections from process.cpp are retained. Vptr checks on
        # discarded process services otherwise require their unrelated RTTI.
        flags = ["-fsanitize=address,undefined", "-fno-sanitize=vptr", "-fno-omit-frame-pointer"] if args.sanitize else []
        subprocess.run([
            *shlex.split(args.cxx), "-std=c++23", "-O2", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
            "-fno-exceptions", "-ffunction-sections", "-fdata-sections", "-DOK_ARCH_TARGET_X86_64", *flags,
            f"-I{root / 'include'}", str(root / "tests/host/linux_exec_tests.cpp"),
            str(root / "src/sched/process.cpp"), "-Wl,--gc-sections", "-o", str(output),
        ], check=True)
        subprocess.run([str(output), *probes], check=True)
        if args.cross:
            cross_compile(root, args.toolchain_root or root / "toolchains", Path(directory))


if __name__ == "__main__":
    main()
