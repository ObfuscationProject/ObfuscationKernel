#!/usr/bin/env python3
"""Build and run an isolated hardware test of the i386 execution backend.

This does not change xmake configuration or link the kernel model test suite.
The test executes ring-3 machine instructions in QEMU and inspects real traps.
"""
from __future__ import annotations

import argparse
import subprocess
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--toolchain", type=Path, required=True)
    parser.add_argument("--qemu", type=Path, default=Path("/usr/bin/qemu-system-i386"))
    parser.add_argument("--output", type=Path)
    parser.add_argument("--kernel-fault", action="store_true", help="Require a distinct supervisor fault diagnostic")
    args = parser.parse_args()
    project = Path(__file__).resolve().parents[1]
    out = args.output or project / "build/native-tests/i386"
    out.mkdir(parents=True, exist_ok=True)
    prefix = args.toolchain.resolve() / "bin/i386-elf-"
    gcc, gxx, ld, objcopy = [str(prefix) + tool for tool in ("gcc", "g++", "ld", "objcopy")]
    flags = ["-std=c++23", "-O2", "-mno-sse", "-mno-sse2", "-ffreestanding", "-fno-builtin", "-fno-exceptions",
             "-fno-stack-protector", "-fno-use-cxa-atexit", "-fno-threadsafe-statics", "-fno-pic", "-fno-pie",
             "-Wall", "-Wextra", "-Werror", "-I", str(project / "include")]
    if args.kernel_fault:
        flags.append("-DOK_NATIVE_TEST_KERNEL_FAULT=1")
    objects = []
    for name, source in (("boot", "src/arch/i386/boot.S"), ("handoff", "src/boot/raw_handoff.cpp"), ("execution", "src/arch/i386/execution.cpp"), ("irq", "src/interrupt/platform_irq.cpp"),
                         ("entry", "src/arch/i386/execution.S"), ("user", "tests/native/i386_execution_user.S"),
                         ("test", "tests/native/i386_execution_test.cpp")):
        target = out / (name + ".o")
        command = [gxx, *flags] if source.endswith(".cpp") else [gcc]
        source_path = project / source
        subprocess.run([*command, "-c", str(source_path), "-o", str(target)], check=True)
        objects.append(str(target))
    elf, payload, image = out/"kernel.elf", out/"payload.bin", out/"kernel.bin"
    subprocess.run([gxx, "-nostdlib", "-static", "-no-pie", "-Wl,--build-id=none",
                    "-Wl,-T,"+str(project/"src/arch/i386/linker.ld"), *objects, "-lgcc", "-o", str(elf)], check=True)
    subprocess.run([objcopy, "-O", "binary", str(elf), str(payload)], check=True)
    boot_object, boot_sector = out/"boot16.o", out/"boot16.bin"
    subprocess.run([gcc, "-c", str(project/"src/arch/x86_64/boot16.S"), "-o", str(boot_object)], check=True)
    subprocess.run([ld, "-Ttext=0x7c00", "--oformat=binary", str(boot_object), "-o", str(boot_sector)], check=True)
    capacity = 512 * 32 * 512
    data = payload.read_bytes()
    if len(data) > capacity:
        raise RuntimeError("native test payload exceeds BIOS capacity")
    image.write_bytes(boot_sector.read_bytes() + data + bytes(capacity-len(data)))
    command = [str(args.qemu), "-cpu", "max", "-m", "128M", "-drive", f"file={image},format=raw,if=ide", "-boot", "c",
               "-display", "none", "-serial", "stdio", "-monitor", "none", "-no-reboot",
               "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04"]
    try:
        result = subprocess.run(command, capture_output=True, text=True, timeout=30)
    except subprocess.TimeoutExpired as error:
        print((error.stdout or b"").decode(errors="replace"))
        raise RuntimeError("native hardware test timed out") from error
    output = result.stdout + result.stderr
    (out/"serial.log").write_text(output)
    print(output, end="")
    expected_exit = 35 if args.kernel_fault else 33
    expected_marker = "OK_NATIVE_KERNEL_FAULT vector=00000006" if args.kernel_fault else "OK_NATIVE_TEST_PASS scope=hardware"
    if result.returncode != expected_exit or expected_marker not in output:
        raise RuntimeError(f"native hardware test failed: QEMU exit {result.returncode}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
