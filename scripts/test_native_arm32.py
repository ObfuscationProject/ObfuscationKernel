#!/usr/bin/env python3
"""Build an isolated ARM32 User-mode/MMU/GIC hardware probe without changing xmake."""
from __future__ import annotations
import argparse
from pathlib import Path
import subprocess

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--toolchain", type=Path)
    parser.add_argument("--qemu", default="qemu-system-arm")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    toolchain = args.toolchain or root / "toolchains/arm-none-eabi"
    compiler = toolchain.resolve() / "bin/arm-none-eabi-g++"
    output = args.output or root / "build/native-tests/arm32"
    output.mkdir(parents=True, exist_ok=True)
    common = ["-march=armv7-a", "-marm", "-mfloat-abi=soft", "-mfpu=neon"]
    cxx = ["-std=c++23", "-O2", "-ffreestanding", "-fno-builtin", "-fno-exceptions", "-fno-stack-protector", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-I", str(root / "include")]
    objects = []
    sources = [("boot", "src/arch/arm32/boot.S"), ("handoff", "src/boot/raw_handoff.cpp"), ("execution", "src/arch/arm32/execution.cpp"), ("bridge", "src/arch/arm32/execution.S"), ("user", "tests/native/arm32_execution_user.S"), ("test", "tests/native/arm32_execution_test.cpp")]
    for name, source in sources:
        target = output / f"{name}.o"
        subprocess.run([str(compiler), *common, *(cxx if source.endswith(".cpp") else []), "-c", str(root / source), "-o", str(target)], check=True)
        objects.append(str(target))
    elf = output / "probe.elf"
    subprocess.run([str(compiler), *common, "-nostdlib", "-static", "-Wl,--build-id=none", "-Wl,-T," + str(root / "src/arch/arm32/linker.ld"), *objects, "-lgcc", "-o", str(elf)], check=True)
    command = [args.qemu, "-machine", "virt", "-m", "128M", "-cpu", "cortex-a15", "-semihosting-config", "enable=on,target=native", "-kernel", str(elf), "-display", "none", "-serial", "stdio", "-monitor", "none", "-no-reboot"]
    try:
        result = subprocess.run(command, capture_output=True, text=True, timeout=20)
    except subprocess.TimeoutExpired as error:
        serial = b"".join(part if isinstance(part, bytes) else (part or "").encode()
                          for part in (error.stdout, error.stderr)).decode(errors="replace")
        (output / "serial.log").write_text(serial)
        print(serial, end="")
        raise RuntimeError("ARM32 native hardware probe timed out") from error
    serial = result.stdout + result.stderr
    (output / "serial.log").write_text(serial)
    print(serial, end="")
    if result.returncode != 0 or "ARM32_NATIVE_PASS scope=hardware" not in serial:
        raise RuntimeError(f"ARM32 native hardware probe failed: QEMU exit {result.returncode}")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
