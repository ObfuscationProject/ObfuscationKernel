#!/usr/bin/env python3
"""Build and run the ppce500 BookE/SPE native hardware probe independently of xmake."""
from __future__ import annotations
import argparse
from pathlib import Path
import selectors
import subprocess
import time


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--toolchain", type=Path, required=True)
    parser.add_argument("--qemu", default="qemu-system-ppc")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--kernel-fault", action="store_true", help="Require a distinct supervisor fault diagnostic")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    compiler = args.toolchain.resolve() / "bin/powerpc-eabi-g++"
    output = args.output or root / "build/native-tests/ppc"
    output.mkdir(parents=True, exist_ok=True)
    common = ["-msoft-float", "-msdata=none", "-fno-pic", "-fno-pie"]
    cxx = ["-std=c++23", "-O2", "-ffreestanding", "-fno-builtin", "-fno-exceptions",
           "-fno-stack-protector", "-Wall", "-Wextra", "-Werror", "-I", str(root / "include")]
    if args.kernel_fault:
        cxx.append("-DOK_NATIVE_TEST_KERNEL_FAULT=1")
    objects = []
    sources = [("boot", "src/arch/ppc/boot.S"), ("handoff", "src/boot/raw_handoff.cpp"), ("execution", "src/arch/ppc/execution.cpp"), ("irq", "src/interrupt/platform_irq.cpp"),
               ("bridge", "src/arch/ppc/execution.S"), ("user", "tests/native/ppc_execution_user.S"),
               ("test", "tests/native/ppc_execution_test.cpp")]
    for name, source in sources:
        target = output / f"{name}.o"
        subprocess.run([str(compiler), *common, *(cxx if source.endswith(".cpp") else []),
                        "-c", str(root / source), "-o", str(target)], check=True)
        objects.append(str(target))
    elf = output / "probe.elf"
    subprocess.run([str(compiler), *common, "-nostdlib", "-static", "-Wl,--build-id=none",
                    "-Wl,-T," + str(root / "src/arch/ppc/linker.ld"), *objects, "-lgcc", "-o", str(elf)], check=True)
    command = [args.qemu, "-machine", "ppce500", "-m", "256M", "-kernel", str(elf),
               "-display", "none", "-serial", "stdio", "-monitor", "none", "-no-reboot"]
    data = bytearray()
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        with selectors.DefaultSelector() as poll:
            poll.register(process.stdout, selectors.EVENT_READ)
            deadline = time.monotonic() + 20
            while time.monotonic() < deadline:
                if poll.select(0.2):
                    chunk = process.stdout.read1(4096)
                    if not chunk:
                        break
                    data.extend(chunk)
                    if any(marker in data for marker in (b"OK_NATIVE_TEST_PASS scope=hardware architecture=ppc\n", b"OK_NATIVE_TEST_FAIL", b"OK_NATIVE_KERNEL_FAULT architecture=ppc\n")):
                        break
    finally:
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
    serial = data.decode(errors="replace")
    (output / "serial.log").write_text(serial)
    print(serial, end="")
    expected_marker = "OK_NATIVE_KERNEL_FAULT architecture=ppc" if args.kernel_fault else "OK_NATIVE_TEST_PASS scope=hardware architecture=ppc"
    if expected_marker not in serial:
        raise RuntimeError("PPC native hardware probe failed or timed out")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
