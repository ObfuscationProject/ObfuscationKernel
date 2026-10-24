#!/usr/bin/env python3
"""QEMU process-pool startup rejection using exact prebuilt Kernel SDK artifacts.

The missing-handoff case links a test-only capture wrapper. Reduced RAM uses the
original SDK kernel. No kernel source is compiled by this artifact consumer.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import select
import subprocess
import time

import test_native_firmware_handoff as firmware

SUPPORTED = ("i386", "x86_64", "aarch64", "arm32", "rv32", "rv64", "ppc")


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk-matrix", type=Path, required=True,
                        help="Directory containing <architecture>/sdk/manifest.json")
    parser.add_argument("--toolchains", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--arch", choices=SUPPORTED, action="append")
    parser.add_argument("--qemu-dir", type=Path, default=Path("/usr/bin"))
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    rows = []
    skipped = []
    for arch in args.arch or SUPPORTED:
        sdk = args.sdk_matrix.resolve() / arch / "sdk"
        manifest = json.loads((sdk / "manifest.json").read_text())
        if manifest["arch"] != arch:
            raise ValueError("SDK architecture mismatch")
        for name, expected in manifest["files"].items():
            path = (sdk / name).resolve()
            if not path.is_relative_to(sdk) or digest(path) != expected:
                raise ValueError("SDK file mismatch: " + name)
        folder = output / arch
        folder.mkdir()
        prefix = args.toolchains.resolve() / manifest["triple"] / "bin" / manifest["triple"]
        tools = {name: Path(str(prefix) + "-" + name) for name in ("g++", "objcopy", "nm")}
        source = folder / "omit_capture.cpp"
        source.write_text('extern "C" void __wrap_ok_boot_capture(__UINTPTR_TYPE__,__UINTPTR_TYPE__,'
                          '__UINTPTR_TYPE__,__UINTPTR_TYPE__) noexcept {}\n')
        commands = []

        def run(command):
            command = list(map(str, command))
            commands.append(command)
            with (folder / "build.log").open("a") as log:
                subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)

        run([tools["g++"], "-std=c++23", *manifest["cxxflags"], "-c", source,
             "-o", folder / "omit_capture.o"])
        run([tools["g++"], *manifest["ldflags"], "-nostdlib", "-static", "-no-pie",
             "-Wl,--build-id=none", "-Wl,-z,max-page-size=0x1000", "-Wl,--wrap=ok_boot_capture",
             "-Wl,-T," + str(sdk / "boot/linker.ld"), folder / "omit_capture.o", "-Wl,--whole-archive",
             sdk / "lib/libokernel_boot.a", sdk / "lib/libokernel.a", "-Wl,--no-whole-archive",
             "-lgcc", "-o", folder / "missing.elf"])
        if manifest["image_format"] == "elf":
            image = (folder / "missing.elf").read_bytes()
        else:
            run([tools["objcopy"], "-O", "binary", folder / "missing.elf", folder / "payload.bin"])
            image = (folder / "payload.bin").read_bytes()
            if manifest["image_format"] == "bios-disk":
                capacity = manifest.get("bios_payload_bytes", 4 * 1024 * 1024)
                if capacity not in (4 * 1024 * 1024, 8 * 1024 * 1024) or len(image) > capacity:
                    raise ValueError("BIOS payload exceeds loader bound")
                image = (sdk / "boot/boot16.bin").read_bytes() + image + bytes(capacity - len(image))
        (folder / "missing.bin").write_bytes(image)
        symbols = {parts[-1]: int(parts[0], 16)
                   for line in subprocess.check_output([str(tools["nm"]), "-n", str(sdk / "image/kernel.elf")], text=True).splitlines()
                   if len(parts := line.split()) == 3 and parts[-1] in ("_kernel_start", "_kernel_end")}
        base = 0x40000000 if arch in ("aarch64", "arm32") else 0x80000000 if arch in ("rv32", "rv64") else 0
        megabytes = math.ceil((symbols["_kernel_end"] - base) / (1024 * 1024)) + 1
        # RISC-V's generated DTB is placed below a 2 MiB boundary. Round upward
        # so the firmware blob does not overlap the full ELF NOLOAD extent.
        if arch in ("rv32", "rv64"):
            megabytes = 2 * math.ceil((symbols["_kernel_end"] - base) / (2 * 1024 * 1024)) + 2
        cases = [("missing", folder / "missing.bin", None)]
        if arch == "arm32":
            cases.append(("direct-elf", sdk / "image/kernel.elf", None))
        if arch == "ppc":
            skipped.append({"architecture": arch, "case": "scarce", "reason":
                            "ppce500 RAM and DTB granularity cannot leave less than 4 MiB after this image"})
        else:
            cases.append(("scarce", sdk / "image/kernel.bin", str(megabytes) + "M"))
        for kind, kernel, ram in cases:
            qemu = args.qemu_dir / ("qemu-system-" + {"arm32": "arm", "rv32": "riscv32", "rv64": "riscv64"}.get(arch, arch))
            command = firmware.qemu_command(qemu, arch, kernel)
            if ram:
                if "-m" in command:
                    command[command.index("-m") + 1] = ram
                else:
                    command += ["-m", ram]
            child = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            data = bytearray()
            deadline = time.monotonic() + 8
            try:
                while time.monotonic() < deadline:
                    if select.select([child.stdout], [], [], 0.1)[0]:
                        chunk = os.read(child.stdout.fileno(), 65536)
                        if not chunk:
                            break
                        data += chunk
            finally:
                child.terminate()
                try:
                    data += child.communicate(timeout=2)[0]
                except subprocess.TimeoutExpired:
                    child.kill()
                    data += child.communicate()[0]
            log = folder / (kind + ".log")
            log.write_bytes(data)
            passed = (data.count(b"OK_NATIVE_MEMORY stage=prepare source=firmware status=fail") == 1 and
                      not any(marker in data for marker in (b"source=linker", b"OK_NATIVE_FRAMES", b"OK_TEST_PASS", b"OK_NATIVE scope=hardware")))
            rows.append({"architecture": arch, "case": kind, "pass": passed, "memory": ram,
                         "sdk_manifest_sha256": digest(sdk / "manifest.json"), "commands": commands,
                         "qemu": list(map(str, command)), "serial_sha256": digest(log),
                         "kernel_sha256": digest(kernel), "wrapper_sha256": digest(source)})
            report = {"scope": "actual QEMU process-memory startup rejection", "rows": rows, "skipped": skipped,
                      "driver_sha256": digest(Path(__file__)), "recipe_sha256": digest(Path(firmware.__file__))}
            (output / "results.json").write_text(json.dumps(report, indent=2) + "\n")
            print(arch, kind, "PASS" if passed else "FAIL", flush=True)
            if not passed:
                raise RuntimeError("startup rejection was not observed: " + str(log))


if __name__ == "__main__":
    main()
