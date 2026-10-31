#!/usr/bin/env python3
"""Compile and execute the AArch64 EL0/MMU/SVC/timer hardware tests in QEMU.

The independent build does not change any xmake configuration. Both supported
Linux Image boot entry levels, EL1 and EL2, execute the same EL0 test program.
"""
from __future__ import annotations

import argparse
import subprocess
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--toolchain", type=Path, required=True)
    parser.add_argument("--qemu", type=Path, default=Path("/usr/bin/qemu-system-aarch64"))
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    project = Path(__file__).resolve().parents[1]
    out = args.output or project / "build/native-tests/aarch64"
    out.mkdir(parents=True, exist_ok=True)
    prefix = args.toolchain.resolve() / "bin/aarch64-elf-"
    gcc, gxx, objcopy = [str(prefix) + tool for tool in ("gcc", "g++", "objcopy")]
    flags = ["-std=c++23", "-O2", "-ffreestanding", "-fno-builtin", "-fno-exceptions",
             "-fno-stack-protector", "-fno-use-cxa-atexit", "-fno-threadsafe-statics", "-fno-pic", "-fno-pie",
             "-mgeneral-regs-only", "-mstrict-align", "-mno-outline-atomics", "-Wall", "-Wextra", "-Werror",
             "-I", str(project / "include"), "-DOK_ARCH_TARGET_AARCH64"]
    objects = []
    for name, source in (("boot", "src/arch/aarch64/boot.S"), ("handoff", "src/boot/raw_handoff.cpp"), ("execution", "src/arch/aarch64/execution.cpp"), ("irq", "src/interrupt/platform_irq.cpp"),
                         ("clock", "src/arch/clock.cpp"), ("entry", "src/arch/aarch64/execution.S"), ("user", "tests/native/aarch64_execution_user.S"),
                         ("test", "tests/native/aarch64_execution_test.cpp")):
        target = out / (name + ".o")
        command = [gxx, *flags] if source.endswith(".cpp") else [gcc]
        subprocess.run([*command, "-c", str(project/source), "-o", str(target)], check=True)
        objects.append(str(target))
    elf, image = out/"kernel.elf", out/"kernel.bin"
    subprocess.run([gxx, "-nostdlib", "-static", "-no-pie", "-Wl,--build-id=none",
                    "-Wl,-T,"+str(project/"src/arch/aarch64/linker.ld"), *objects, "-lgcc", "-o", str(elf)], check=True)
    subprocess.run([objcopy, "-O", "binary", str(elf), str(image)], check=True)
    for entry_level, machine in (("el1", "virt,gic-version=2"), ("el2", "virt,gic-version=2,virtualization=on")):
        command = [str(args.qemu), "-machine", machine, "-cpu", "cortex-a57", "-m", "256M",
                   "-kernel", str(image), "-display", "none", "-serial", "stdio", "-monitor", "none",
                   "-no-reboot", "-semihosting-config", "enable=on,target=native"]
        try:
            result = subprocess.run(command, capture_output=True, text=True, timeout=20)
            output = result.stdout + result.stderr
        except subprocess.TimeoutExpired as error:
            output = (error.stdout or b"").decode(errors="replace") + (error.stderr or b"").decode(errors="replace")
            (out/f"serial-{entry_level}.log").write_text(output)
            print(output, end="")
            raise RuntimeError(f"native {entry_level} hardware test timed out") from error
        (out/f"serial-{entry_level}.log").write_text(output)
        print(f"entry_level={entry_level}\n{output}", end="")
        if result.returncode != 0 or "OK_NATIVE_TEST_PASS scope=hardware architecture=aarch64" not in output:
            raise RuntimeError(f"native {entry_level} hardware test failed: QEMU exit {result.returncode}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
