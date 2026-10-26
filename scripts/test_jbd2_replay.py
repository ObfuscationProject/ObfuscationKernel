#!/usr/bin/env python3
"""Exercise actual replay I/O against an injected block device, as a host model."""
import argparse
import os
from pathlib import Path
import shlex
import struct
import subprocess
import sys
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
    "aarch64": ["-mno-outline-atomics", "-mgeneral-regs-only", "-mstrict-align"], "arm32": ["-march=armv7-a", "-marm", "-mno-unaligned-access"],
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
            "-DOK_ARCH_TARGET_" + name.upper(), f"-I{root / 'include'}", "-c",
            str(root / "tests/host/jbd2_replay_compile.cpp"), "-o", str(output),
        ], check=True)
        data = output.read_bytes()
        order = "<" if endian == 1 else ">"
        if (data[:4] != b"\x7fELF" or data[4:6] != bytes((elf_class, endian)) or
                struct.unpack_from(order + "H", data, 18)[0] != machine):
            raise ValueError("wrong isolated object ABI: " + str(output))
        print(f"PASS isolated freestanding JBD2 replay compile {name} (not executed)", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--cross", action="store_true")
    parser.add_argument("--toolchain-root", type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    subprocess.run([sys.executable, str(root / "scripts/generate_jbd2_fixture.py"), "--check"], check=True)
    with tempfile.TemporaryDirectory(prefix="ok-jbd2-replay-") as directory:
        binary = Path(directory) / "jbd2-replay"
        command = [
            *shlex.split(args.cxx), "-std=c++23", "-O2", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
            "-fno-exceptions", "-DOK_ARCH_TARGET_X86_64", f"-I{root / 'include'}",
            str(root / "tests/host/jbd2_replay_tests.cpp"), "-Wl,-l:libext2fs.so.2", "-o", str(binary),
        ]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True)
        subprocess.run([str(binary), str(root / "tests/fixtures/jbd2_v3.bin")], check=True)
        if args.cross:
            cross_compile(root, args.toolchain_root or root / "toolchains", Path(directory))


if __name__ == "__main__":
    main()
