#!/usr/bin/env python3
"""Qualify read-only runtime journal binding using hosted e2fsprogs fixtures.

This does not boot a guest, replay a journal, publish a mount or test power loss.
An explicit kernel root supports isolated candidate validation before integration.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import struct
import subprocess
import sys
import time

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


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    source = Path(__file__).resolve().parents[1]
    parser.add_argument("--kernel-root", type=Path, default=source)
    parser.add_argument("--toolchain-root", type=Path)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--cross", action="store_true")
    parser.add_argument("--linux-dirty-image", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    root, output = args.kernel_root.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    commands = []
    report = {"scope": "hosted-ext4-runtime-recovery-binding", "status": "running", "commands": commands,
              "native_execution": False, "mount_admission": False, "journal_replay": False,
              "sanitize": args.sanitize, "asan_options": os.environ.get("ASAN_OPTIONS", "")}
    def save(): (output / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    def run(command, label, cwd=None):
        command = [str(x) for x in command]
        record = {"argv": command, "label": label, "cwd": str(cwd) if cwd else None, "status": "running"}
        commands.append(record); save()
        start = time.monotonic()
        result = subprocess.run(command, text=True, capture_output=True, cwd=cwd)
        (output / (label + ".log")).write_text(result.stdout + result.stderr)
        record.update(status="passed" if result.returncode == 0 else "failed", returncode=result.returncode,
                      seconds=time.monotonic() - start)
        save()
        if result.returncode: raise RuntimeError(label + " failed; inspect retained log")
        return result.stdout + result.stderr
    save()
    try:
        frozen = output / "sources"
        shutil.copytree(root / "include", frozen / "include")
        for relative in ("include/ok/fs/ext4_recovery.hpp", "src/fs/ext4_recovery.cpp",
                         "tests/host/ext4_recovery_tests.cpp", "tests/host/ext4_recovery_compile.cpp",
                         "scripts/test_ext4_recovery.py"):
            destination = frozen / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes((source / relative).read_bytes())
        report["source_sha256"] = {str(p.relative_to(frozen)): digest(p) for p in sorted(frozen.rglob("*")) if p.is_file()}
        implementation = frozen / "src/fs/ext4_recovery.cpp"
        binary = output / "ext4-recovery-tests"
        flags = ["-std=c++23", "-O2", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-fno-exceptions", "-frtti"]
        run([*shlex.split(args.cxx), *flags, "-DOK_ARCH_TARGET_X86_64", "-I" + str(frozen / "include"),
             frozen / "tests/host/ext4_recovery_tests.cpp", implementation, "-Wl,-l:libext2fs.so.2",
             *(["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if args.sanitize else []),
             "-o", binary], "compile-hosted")
        for size, megabytes, groups, journal_mb in ((1024, 32, None, 4), (2048, 32, None, 4),
                                                    (4096, 32, None, 4), (1024, 64, 2048, 1)):
            label = str(size) + ("-max-groups" if groups else "")
            fixture = output / ("fixture-" + label)
            fixture.mkdir()
            disk = fixture / "ext4.img"
            with disk.open("xb") as image: image.truncate(megabytes * 1024 * 1024)
            run(["mke2fs", "-F", "-q", "-t", "ext4", "-b", size, "-I", 256, "-O",
                 "none,has_journal,extents,filetype,metadata_csum", "-J", "size=" + str(journal_mb), *(["-g", groups] if groups else []), "-E",
                 "lazy_itable_init=0,lazy_journal_init=0", "-m", 0, disk], "format-" + label)
            (fixture / "seed.bin").write_bytes(bytes((index * 17 + 29) & 255 for index in range(4096)))
            script = fixture / "fixture.debugfs"
            script.write_text("write seed.bin /seed\njournal_open -c -v 3\njournal_close\n")
            run(["debugfs", "-w", "-f", script, disk], "prepare-" + label, cwd=fixture)
            run(["e2fsck", "-fn", disk], "fsck-before-" + label)
            independent = run(["debugfs", "-R", "stat <8>", disk], "journal-stat-" + label)
            before = digest(disk)
            actual = run([binary, disk], "hosted-" + label)
            if before != digest(disk): raise ValueError("read-only admission changed fixture")
            extent_line = re.search(r"EXTENTS:\s*([^\n]+)", independent)
            match = re.fullmatch(r"\(0-(\d+)\):(\d+)-(\d+)", extent_line[1].strip()) if extent_line else None
            if not match: raise ValueError("reference journal is not exactly one inline initialized extent")
            count, first, end = map(int, match.groups()); count += 1
            observed = re.search(r"MAPPING block_size=(\d+) journal_first=(\d+) journal_blocks=(\d+)", actual)
            if not observed or tuple(map(int, observed.groups())) != (size, first, count) or end - first + 1 != count:
                raise ValueError("runtime mapping differs from independent debugfs extent")
            if groups and "metadata_ranges=128" not in actual:
                raise ValueError("max-group fixture did not exercise all 128 metadata ranges")
            run(["e2fsck", "-fn", disk], "fsck-after-" + label)
            print(actual, end="", flush=True)
        if args.linux_dirty_image:
            disk = output / "linux-dirty-capture.img"
            shutil.copyfile(args.linux_dirty_image, disk)
            before = digest(disk)
            actual = run([binary, disk], "hosted-linux-dirty")
            if before != digest(disk): raise ValueError("admission changed Linux capture")
            if "needs_recovery=1 empty=0" not in actual: raise ValueError("not a nonempty dirty Linux capture")
            report["linux_capture_sha256"] = before
            print(actual, end="", flush=True)
        if args.cross:
            toolchains = args.toolchain_root.resolve() if args.toolchain_root else root / "toolchains"
            for name, triple, elf_class, endian, machine in PROFILES:
                compiler = toolchains / triple / "bin" / (triple + "-g++")
                if not compiler.is_file(): raise ValueError("explicit compiler absent: " + str(compiler))
                for label, translation in (("implementation", implementation),
                    ("consumer", frozen / "tests/host/ext4_recovery_compile.cpp")):
                    obj = output / (name + "-" + label + ".o")
                    run([compiler, *flags, "-ffreestanding", "-fno-threadsafe-statics", "-fno-use-cxa-atexit",
                         *TARGET_FLAGS[name], "-DOK_ARCH_TARGET_" + name.upper(), "-I" + str(frozen / "include"),
                         "-c", translation, "-o", obj], "cross-" + name + "-" + label)
                    data = obj.read_bytes()
                    order = "<" if endian == 1 else ">"
                    if data[:4] != b"\x7fELF" or data[4:6] != bytes((elf_class, endian)) or \
                            struct.unpack_from(order + "H", data, 18)[0] != machine:
                        raise ValueError("wrong target object: " + str(obj))
                print("PASS freestanding ext4 recovery compile " + name + " (not executed)", flush=True)
        report["status"] = "passed"
    except Exception as error:
        report.update(status="failed", error=str(error))
        print("FAIL " + str(error), file=sys.stderr)
    finally:
        report["artifact_sha256"] = {str(p.relative_to(output)): digest(p) for p in sorted(output.rglob("*"))
                                      if p.is_file() and p.name != "result.json"}
        save()
    return 0 if report["status"] == "passed" else 1


if __name__ == "__main__": raise SystemExit(main())
