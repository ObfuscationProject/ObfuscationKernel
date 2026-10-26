#!/usr/bin/env python3
"""Check service-stack coordinates, ten native compile/link profiles and x86 QEMU SPs.

The fixture uses the canonical boot sources. It does not validate permissions,
faulting guards, CPU identity, SMP or complete service/IRQ/fault stack bounds.
"""
from pathlib import Path
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess

PROFILES = {
    "i386": ("i386-elf", ["-mno-sse", "-mno-sse2"]),
    "x86_64": ("x86_64-elf", ["-mno-red-zone"]),
    "aarch64": ("aarch64-elf", ["-mno-outline-atomics", "-mgeneral-regs-only", "-mstrict-align"]),
    "arm32": ("arm-none-eabi", ["-march=armv7-a", "-marm", "-mno-unaligned-access"]),
    "rv64": ("riscv64-elf", ["-mcmodel=medany"]),
    "rv32": ("riscv32-elf", ["-mcmodel=medany"]),
    "loongarch64": ("loongarch64-elf", ["-msimd=none"]),
    "mips": ("mips-elf", ["-march=mips32r2", "-G0"]),
    "mips64": ("mips64-elf", ["-march=mips64r2", "-mabi=64", "-G0"]),
    "ppc": ("powerpc-eabi", ["-msoft-float", "-msdata=none"]),
}
COMMON = ("include/ok/core/types.hpp", "include/ok/arch/types.hpp", "include/ok/arch/native.hpp",
          "include/ok/arch/profiles.hpp", "include/ok/arch/arch.hpp", "include/ok/arch/instantaneous_sp.hpp",
          "include/ok/arch/stack_bounds.hpp", "include/ok/arch/service_stack.hpp",
          "include/ok/user/native_signal_config.hpp", "tests/host/stack_bounds_tests.cpp",
          "tests/native/service_stack_probe.cpp", "src/arch/x86_64/boot16.S")


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def lua_args(values):
    return ", ".join(json.dumps(str(value)) for value in values)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--toolchains", type=Path, help="Explicit directory containing the ten named toolchain roots")
    parser.add_argument("--host-cxx", type=Path, default=Path("/usr/bin/g++"))
    parser.add_argument("--qemu", type=Path, help="Explicit x86_64 QEMU; defaults to the named toolchain executable")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    source = output / "source"
    toolchains = (args.toolchains or root / "toolchains").resolve()
    xmake = Path(shutil.which("xmake") or "").resolve()
    host = args.host_cxx.resolve()
    if not xmake.is_file() or not host.is_file():
        raise RuntimeError("xmake and an explicit host C++ compiler are required")
    names = list(COMMON) + [str(Path(__file__).resolve().relative_to(root))]
    for arch in PROFILES:
        names += [f"src/arch/{arch}/boot.S", f"src/arch/{arch}/linker.ld"]
    state = dict(status="preparing", source_sha256={}, commands=[], host_rows=[], native_rows=[],
                 guest_rows=[], mapped_permissions=False, faulting_guards=False, smp=False,
                 complete_stack_graph=False, sdk_consumption=False, full_kernel_boot=False)
    env = dict(os.environ, XMAKE_ROOT="y", XMAKE_GLOBALDIR=str(output / "xmake-global"),
               ASAN_OPTIONS="detect_leaks=0:halt_on_error=1", UBSAN_OPTIONS="halt_on_error=1")
    for name in ("CPATH", "CPLUS_INCLUDE_PATH", "C_INCLUDE_PATH", "LIBRARY_PATH", "GCC_EXEC_PREFIX", "COMPILER_PATH"):
        env.pop(name, None)

    def save():
        (output / "result.json").write_text(json.dumps(state, indent=2) + "\n")

    def run(argv, label, expected=0, timeout=60):
        argv = list(map(str, argv))
        result = subprocess.run(argv, cwd=source, env=env, capture_output=True, timeout=timeout)
        log = output / (label + ".log")
        with log.open("xb") as stream:
            stream.write(result.stdout + result.stderr)
        state["commands"].append(dict(argv=argv, returncode=result.returncode, log=str(log), sha256=sha(log)))
        save()
        if result.returncode != expected:
            raise RuntimeError(f"{label} exited {result.returncode}; see {log}")
        return result.stdout.decode(errors="replace")

    def build_record(name):
        return json.loads((source / "records" / (name + ".json")).read_text())

    try:
        for name in names:
            target = source / name
            target.parent.mkdir(parents=True, exist_ok=True)
            before = sha(root / name)
            shutil.copy2(root / name, target)
            if sha(target) != before or sha(root / name) != before:
                raise RuntimeError("source changed while copying: " + name)
            state["source_sha256"][name] = before
        state["tools"] = {str(xmake): sha(xmake), str(host): sha(host)}
        lines = ['set_project("service-stack-isolated-checks")', 'set_languages("c++23")',
                 'toolchain("stack-host")', 'set_kind("standalone")',
                 'set_toolset("cxx", ' + json.dumps(str(host)) + ')',
                 'set_toolset("ld", ' + json.dumps(str(host)) + ')', 'toolchain_end()']
        record_hook = ['after_build(function (target)', 'import("core.base.json")',
                       'os.mkdir("records")', 'json.savefile(path.join("records", target:name() .. ".json"),',
                       '{target = target:targetfile(), objects = target:objectfiles()})', 'end)']
        for sanitize in (False, True):
            name = "host-sanitize" if sanitize else "host-plain"
            flags = ["-O1", "-g", "-Wall", "-Wextra", "-Wpedantic", "-Werror"]
            ldflags = []
            if sanitize:
                flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-pie"]
                ldflags += ["-fsanitize=address,undefined", "-no-pie"]
            lines += [f'target("{name}")', 'set_kind("binary")', 'set_toolchains("stack-host")',
                      'set_targetdir("artifacts")', 'add_includedirs("include")',
                      'add_files("tests/host/stack_bounds_tests.cpp")',
                      'add_cxxflags(' + lua_args(flags) + ', {force=true})']
            if ldflags:
                lines += ['add_ldflags(' + lua_args(ldflags) + ', {force=true})']
            lines += record_hook + ['target_end()']
        common_flags = ["-ffreestanding", "-fno-exceptions", "-fno-rtti", "-fno-builtin", "-fno-stack-protector",
                        "-fno-threadsafe-statics", "-fno-use-cxa-atexit", "-fno-pic", "-fno-pie",
                        "-Wall", "-Wextra", "-Werror"]
        for arch, (triple, profile_flags) in PROFILES.items():
            prefix = toolchains / triple / "bin" / (triple + "-")
            tools = {tool: Path(str(prefix) + tool).resolve() for tool in ("gcc", "g++", "objcopy", "readelf", "ld")}
            for tool in tools.values():
                if not tool.is_file():
                    raise RuntimeError("missing explicit tool: " + str(tool))
                state["tools"][str(tool)] = sha(tool)
            lines += [f'toolchain("stack-{arch}")', 'set_kind("standalone")',
                      'set_toolset("cxx", ' + json.dumps(str(tools["g++"])) + ')',
                      'set_toolset("as", ' + json.dumps(str(tools["gcc"])) + ')',
                      'set_toolset("ld", ' + json.dumps(str(tools["g++"])) + ')', 'toolchain_end()']
            for mode in ("debug", "release"):
                flags = common_flags + profile_flags + (["-O0", "-g"] if mode == "debug" else ["-O2", "-DNDEBUG"])
                ldflags = ["-nostdlib", "-static", "-no-pie", "-Wl,--build-id=none",
                           f"-Wl,-T,src/arch/{arch}/linker.ld", *profile_flags, "-lgcc"]
                lines += [f'target("{arch}-{mode}")', 'set_kind("binary")',
                          f'set_toolchains("stack-{arch}")', 'set_targetdir("artifacts")',
                          'set_extension(".elf")', 'add_includedirs("include")',
                          'add_defines("OK_KERNEL_FREESTANDING", "OK_ARCH_TARGET_' + arch.upper() + '")',
                          f'add_files("src/arch/{arch}/boot.S", "tests/native/service_stack_probe.cpp")',
                          'add_cxxflags(' + lua_args(flags) + ', {force=true})',
                          'add_asflags(' + lua_args(["-Iinclude", *profile_flags]) + ', {force=true})',
                          'add_ldflags(' + lua_args(ldflags) + ', {force=true})', *record_hook, 'target_end()']
            if arch == "x86_64":
                lines += ['target("boot16")', 'set_kind("object")', 'set_toolchains("stack-x86_64")',
                          'add_files("src/arch/x86_64/boot16.S")', *record_hook, 'target_end()']
        (source / "xmake.lua").write_text("\n".join(lines) + "\n")
        state["xmake_project_sha256"] = sha(source / "xmake.lua")
        state["status"] = "building"
        save()
        run([xmake, "f", "-P", source, "-c", "-y", "--ccache=n"], "configure")
        run([xmake, "build", "-P", source, "-j2", "-v"], "build", timeout=180)
        for name in ("host-plain", "host-sanitize"):
            record = build_record(name)
            binary = source / record["target"]
            text = run([binary], name)
            if not re.fullmatch(r"PASS stack-bounds checks=\d+ scope=hosted-coordinates guards=0\n", text):
                raise RuntimeError("host completion marker is missing")
            state["host_rows"].append(dict(name=name, binary_sha256=sha(binary), marker=text.strip()))
        for arch, (triple, _) in PROFILES.items():
            prefix = toolchains / triple / "bin" / (triple + "-")
            for mode in ("debug", "release"):
                name = arch + "-" + mode
                record = build_record(name)
                binary = source / record["target"]
                text = run([str(prefix) + "readelf", "-sW", binary], name + "-symbols")
                symbols = {}
                for line in text.splitlines():
                    fields = line.split()
                    if len(fields) >= 8 and fields[-1] in ("ok_service_stack_lower", "ok_service_stack_top", "boot_stack", "boot_stack_top"):
                        symbols[fields[-1]] = int(fields[1], 16)
                if (len(symbols) != 4 or symbols["ok_service_stack_lower"] != symbols["boot_stack"] or
                        symbols["ok_service_stack_top"] != symbols["boot_stack_top"] or
                        symbols["ok_service_stack_top"] - symbols["ok_service_stack_lower"] != 1048576):
                    raise RuntimeError("boot aliases do not identify the actual 1 MiB stack: " + name)
                state["native_rows"].append(dict(name=name, scope="compile-link-only", binary_sha256=sha(binary), symbols=symbols))
        triple = "x86_64-elf"
        prefix = toolchains / triple / "bin" / (triple + "-")
        record = build_record("boot16")
        if len(record["objects"]) != 1:
            raise RuntimeError("boot16 must have one explicit object")
        sector = output / "boot16.bin"
        run([str(prefix) + "ld", "-Ttext=0x7c00", "--oformat=binary", source / record["objects"][0], "-o", sector], "boot16-convert")
        qemu = (args.qemu or toolchains / triple / "bin/qemu-system-x86_64").resolve()
        state["tools"][str(qemu)] = sha(qemu)
        for mode in ("debug", "release"):
            elf = source / build_record("x86_64-" + mode)["target"]
            payload = output / (mode + "-payload.bin")
            run([str(prefix) + "objcopy", "-O", "binary", elf, payload], mode + "-payload")
            data = payload.read_bytes()
            capacity = 512 * 32 * 512
            if sector.stat().st_size != 512 or len(data) > capacity:
                raise RuntimeError("BIOS fixture dimensions are invalid")
            disk = output / (mode + "-boot.img")
            disk.write_bytes(sector.read_bytes() + data + bytes(capacity - len(data)))
            text = run([qemu, "-m", "128M", "-drive", f"file={disk},format=raw,if=ide", "-boot", "c",
                        "-display", "none", "-serial", "stdio", "-monitor", "none", "-no-reboot",
                        "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04"], mode + "-qemu", expected=33, timeout=30)
            if text != "PASS service-stack native=1 guards=0\n":
                raise RuntimeError("native completion marker differs")
            state["guest_rows"].append(dict(architecture="x86_64", mode=mode, image_sha256=sha(disk), marker=text.strip()))
        for name, digest in state["source_sha256"].items():
            if sha(source / name) != digest or sha(root / name) != digest:
                raise RuntimeError("source changed during test: " + name)
        if sha(source / "xmake.lua") != state["xmake_project_sha256"]:
            raise RuntimeError("xmake project changed during test")
        for path, digest in state["tools"].items():
            if sha(path) != digest:
                raise RuntimeError("tool changed during test: " + path)
        state["status"] = "passed"
        save()
    except BaseException as error:
        state.update(status="failed", error=repr(error))
        save()
        raise
    print("PASS service-stack: 2 hosted, 20 compile/link rows, 2 x86_64 native guests; guards=0")


if __name__ == "__main__":
    main()
