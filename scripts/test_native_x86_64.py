#!/usr/bin/env python3
"""Build and run an isolated hardware test of the x86_64 execution backend.

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
    parser.add_argument("--qemu", type=Path, default=Path("/usr/bin/qemu-system-x86_64"))
    parser.add_argument("--output", type=Path)
    parser.add_argument("--poison-bootstrap-tables", action="store_true",
                        help="Fill the three boot page tables with nonzero RAM before bootstrap initialization")
    args = parser.parse_args()
    project = Path(__file__).resolve().parents[1]
    out = args.output or project / "build/native-tests/x86_64"
    out.mkdir(parents=True, exist_ok=True)
    prefix = args.toolchain.resolve() / "bin/x86_64-elf-"
    gcc, gxx, ld, objcopy = [str(prefix) + tool for tool in ("gcc", "g++", "ld", "objcopy")]
    flags = ["-std=c++23", "-O2", "-mno-red-zone", "-ffreestanding", "-fno-builtin", "-fno-exceptions",
             "-fno-stack-protector", "-fno-use-cxa-atexit", "-fno-threadsafe-statics", "-fno-pic", "-fno-pie",
             "-Wall", "-Wextra", "-Werror", "-I", str(project / "include")]
    objects = []
    for name, source in (("boot", "src/arch/x86_64/boot.S"), ("execution", "src/arch/x86_64/execution.cpp"),
                         ("entry", "src/arch/x86_64/execution.S"), ("user", "tests/native/x86_64_execution_user.S"),
                         ("test", "tests/native/x86_64_execution_test.cpp")):
        target = out / (name + ".o")
        command = [gxx, *flags] if source.endswith(".cpp") else [gcc]
        source_path = project / source
        if name == "boot" and args.poison_bootstrap_tables:
            boot_source = source_path.read_text()
            if boot_source.count("_start:\n") != 1:
                raise RuntimeError("cannot locate the unique bootstrap entry for RAM fault injection")
            # Inject before the production initializer, after the BIOS has
            # finished loading its padded payload. Production boot has no test
            # hooks and the normal native hardware assertions remain intact.
            injection = ("_start:\n    cld\n    mov $boot_pml4, %edi\n"
                         "    mov $0xa5a5a5a5, %eax\n    mov $3072, %ecx\n"
                         "    rep stosl\n")
            source_path = out / "boot-poisoned.S"
            source_path.write_text(boot_source.replace("_start:\n", injection, 1))
        subprocess.run([*command, "-c", str(source_path), "-o", str(target)], check=True)
        objects.append(str(target))
    elf, payload, image = out/"kernel.elf", out/"payload.bin", out/"kernel.bin"
    subprocess.run([gxx, "-nostdlib", "-static", "-no-pie", "-Wl,--build-id=none",
                    "-Wl,-T,"+str(project/"src/arch/x86_64/linker.ld"), *objects, "-lgcc", "-o", str(elf)], check=True)
    subprocess.run([objcopy, "-O", "binary", str(elf), str(payload)], check=True)
    boot_object, boot_sector = out/"boot16.o", out/"boot16.bin"
    subprocess.run([gcc, "-c", str(project/"src/arch/x86_64/boot16.S"), "-o", str(boot_object)], check=True)
    subprocess.run([ld, "-Ttext=0x7c00", "--oformat=binary", str(boot_object), "-o", str(boot_sector)], check=True)
    capacity = 256 * 32 * 512
    data = payload.read_bytes()
    if len(data) > capacity:
        raise RuntimeError("native test payload exceeds BIOS capacity")
    image.write_bytes(boot_sector.read_bytes() + data + bytes(capacity-len(data)))
    command = [str(args.qemu), "-m", "128M", "-drive", f"file={image},format=raw,if=ide", "-boot", "c",
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
    if result.returncode != 33 or "OK_NATIVE_TEST_PASS scope=hardware" not in output:
        raise RuntimeError(f"native hardware test failed: QEMU exit {result.returncode}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
