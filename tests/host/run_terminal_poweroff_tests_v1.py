#!/usr/bin/env python3
"""Build hosted terminal models with xmake and inspect linked native wrapper bytes.

This is not native instruction execution, cross-ISA execution, guest shutdown,
storage qualification, an SDK consumer check, or a complete milestone gate.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import signal
import struct
import subprocess
import sys
import time


PROFILES = (
    "i386", "x86_64", "aarch64", "arm32", "rv64", "rv32",
    "loongarch64", "mips", "mips64", "ppc",
)
TERMINAL_INPUTS = (
    "src/arch/terminal_power.cpp",
    "tests/host/terminal_power_tests.cpp",
    "include/ok/arch/terminal_power.hpp",
    "include/ok/arch/pc8042_reset.hpp",
    "include/ok/arch/piix4_pm_poweroff.hpp",
    "include/ok/arch/arch.hpp",
    "include/ok/arch/native.hpp",
    "include/ok/arch/profiles.hpp",
    "include/ok/arch/types.hpp",
    "include/ok/core/types.hpp",
    "include/ok/core/power.hpp",
)
POLICY = "OK_NATIVE_QEMU_PIIX4_POWEROFF"
REFUSAL = "QEMU PIIX4 PM poweroff requires the explicit x86_64 policy value 1"
WRAPPER = "ok_arch_piix4_pm_control_write"
OUTW = "ok_arch_piix4_pm_control_outw"
HOLD = "ok_arch_terminal_hold"
WRAPPER_BYTES = bytes.fromhex("66 89 fa 66 89 f0 66 ef c3")
HOLD_BYTES = bytes.fromhex("fa f4 eb fc")
MAX_ELF_BYTES = 64 * 1024 * 1024


def sha(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while block := stream.read(1024 * 1024):
            digest.update(block)
    return digest.hexdigest()


def exact_file(value):
    path = Path(value)
    if not path.is_absolute():
        raise ValueError("explicit absolute file path required: " + str(path))
    path = path.resolve(strict=True)
    if not path.is_file():
        raise ValueError("regular file required: " + str(path))
    return path


def live_group_members(group):
    """Bound /proc inventory and return this session/group's non-zombie members."""
    members = []
    deadline = time.monotonic() + 3
    with os.scandir("/proc") as entries:
        for count, entry in enumerate(entries):
            if count >= 131072 or time.monotonic() > deadline:
                raise RuntimeError("bounded /proc inventory did not complete")
            if not entry.name.isdecimal():
                continue
            try:
                stat = (Path("/proc") / entry.name / "stat").read_text(
                    encoding="utf-8", errors="surrogateescape")
            except (FileNotFoundError, ProcessLookupError):
                continue
            end = stat.rfind(") ")
            if end < 0:
                raise RuntimeError("malformed process stat during group inventory")
            fields = stat[end + 2:].split()
            if len(fields) < 20:
                raise RuntimeError("short process stat during group inventory")
            process_group, session = int(fields[2]), int(fields[3])
            if process_group == group and session == group and fields[0] != "Z":
                members.append({"pid": int(entry.name), "state": fields[0],
                                "pgrp": process_group, "session": session,
                                "starttime_ticks": int(fields[19])})
    return sorted(members, key=lambda member: member["pid"])


def retire_group(process, command):
    """Best-effort signals plus a mandatory bounded final live-member check."""
    retirement = {}
    command["group_retirement"] = retirement
    try:
        retirement["before"] = live_group_members(process.pid)
    except BaseException as error:
        retirement["initial_inventory_error"] = repr(error)
    # A failed initial inventory cannot establish absence; still retire our group.
    for name, sig in (("sigterm", signal.SIGTERM), ("sigkill", signal.SIGKILL)):
        try:
            os.killpg(process.pid, sig)
            retirement["sent_" + name] = True
        except ProcessLookupError:
            retirement["sent_" + name] = False
        deadline = time.monotonic() + 1
        while True:
            process.poll()  # Reap our direct child; grandchildren are not our children.
            try:
                remaining = live_group_members(process.pid)
            except BaseException as error:
                retirement["after_" + name + "_inventory_error"] = repr(error)
                remaining = None
                break
            if not remaining or time.monotonic() >= deadline:
                break
            time.sleep(0.05)
        retirement["after_" + name] = remaining
        if remaining == []:
            break
    command["returncode"] = process.wait(timeout=2)
    retirement["final_live_members"] = live_group_members(process.pid)
    retirement["status"] = "retired" if not retirement["final_live_members"] else "live-members-remain"
    if retirement["final_live_members"]:
        raise RuntimeError("command group still has non-zombie members after bounded retirement")


class LinkedElf:
    """Bounded ELF64 LE x86_64 section/symbol inspection; never execute bytes."""

    def __init__(self, path):
        if path.stat().st_size > MAX_ELF_BYTES:
            raise ValueError("host ELF exceeds bounded artifact size")
        self.data = path.read_bytes()
        if len(self.data) < 64 or self.data[:7] != b"\x7fELF\x02\x01\x01":
            raise ValueError("ELF64 little-endian version 1 required")
        header = struct.unpack_from("<16sHHIQQQIHHHHHH", self.data)
        if header[1] not in (2, 3) or header[2] != 62 or header[3] != 1 or header[8] != 64:
            raise ValueError("linked x86_64 ELF header differs")
        offset, width, count, names_index = header[6], header[11], header[12], header[13]
        if width != 64 or not 1 <= count <= 4096 or not 0 < names_index < count:
            raise ValueError("bounded ordinary ELF section table required")
        self.span(offset, width * count)
        self.sections = [
            struct.unpack_from("<IIQQQQIIQQ", self.data, offset + width * index)
            for index in range(count)
        ]
        for section in self.sections:
            if section[1] != 8:  # SHT_NOBITS has no bytes in the file.
                self.span(section[4], section[5])
        names = self.sections[names_index]
        if names[1] != 3:
            raise ValueError("section names must use a string table")
        names_data = self.span(names[4], names[5])
        self.section_names = [self.cstring(names_data, section[0]) for section in self.sections]
        self.symbols = {}
        tables = 0
        for table_index, table in enumerate(self.sections):
            if table[1] not in (2, 11):  # SHT_SYMTAB and SHT_DYNSYM
                continue
            tables += 1
            if table[9] != 24 or table[5] % 24 or not 0 < table[6] < count:
                raise ValueError("ordinary ELF64 symbol entries and linked string table required")
            strings = self.sections[table[6]]
            if strings[1] != 3:
                raise ValueError("symbol strings must use a string table")
            strings_data = self.span(strings[4], strings[5])
            for at in range(table[4], table[4] + table[5], 24):
                name_offset, info, other, index, address, size = struct.unpack_from("<IBBHQQ", self.data, at)
                name = self.cstring(strings_data, name_offset)
                if name not in (WRAPPER, OUTW, HOLD):
                    continue
                self.symbols.setdefault(name, []).append({
                    "name": name, "info": info, "other": other, "section": index,
                    "address": address, "size": size,
                    "table": self.section_names[table_index],
                })
        if not tables or not any(section[1] == 2 for section in self.sections):
            raise ValueError("unstripped linked ELF with a static symbol table required")

    def span(self, offset, size):
        if offset < 0 or size < 0 or offset > len(self.data) or size > len(self.data) - offset:
            raise ValueError("ELF file range is out of bounds")
        return self.data[offset:offset + size]

    @staticmethod
    def cstring(data, offset):
        if not 0 <= offset < len(data):
            raise ValueError("ELF string offset is out of bounds")
        end = data.find(b"\0", offset)
        if end < 0:
            raise ValueError("ELF string is not terminated")
        # Unrelated linker/compiler names need not use ASCII.
        return data[offset:end].decode("utf-8", errors="surrogateescape")

    def one(self, name, symbol_type, size):
        entries = self.symbols.get(name, [])
        if len(entries) != 1:
            raise ValueError("one exact symbol required across all tables: " + name)
        symbol = entries[0]
        if symbol["info"] >> 4 != 1 or symbol["info"] & 15 != symbol_type or symbol["size"] != size:
            raise ValueError("global symbol binding/type/size differs: " + name)
        index = symbol["section"]
        if not 0 < index < len(self.sections):
            raise ValueError("defined ordinary section symbol required: " + name)
        section = self.sections[index]
        if section[1] != 1 or section[2] & 6 != 6:
            raise ValueError("allocated executable PROGBITS section required: " + name)
        delta = symbol["address"] - section[3]
        if delta < 0 or delta > section[5] or size > section[5] - delta:
            raise ValueError("symbol virtual range is outside its file-backed section: " + name)
        result = dict(symbol)
        result["file_offset"] = section[4] + delta
        result["section_name"] = self.section_names[index]
        return result

    def check_terminal(self, poweroff):
        hold = self.one(HOLD, 2, 4)
        if self.span(hold["file_offset"], 4) != HOLD_BYTES:
            raise ValueError("linked terminal hold instruction bytes differ")
        evidence = {"hold": {**hold, "bytes": HOLD_BYTES.hex()}, "poweroff_enabled": poweroff}
        if not poweroff:
            if self.symbols.get(WRAPPER) or self.symbols.get(OUTW):
                raise ValueError("disabled variant contains a poweroff wrapper or observation label")
            evidence["poweroff_symbols"] = "both absent in an unstripped linked ELF"
            return evidence
        wrapper = self.one(WRAPPER, 2, 9)
        outw = self.one(OUTW, 0, 0)
        if (outw["section"] != wrapper["section"] or
                outw["address"] != wrapper["address"] + 6 or
                outw["file_offset"] != wrapper["file_offset"] + 6):
            raise ValueError("OUTW observation label must be in the wrapper section at +6")
        if self.span(wrapper["file_offset"], 9) != WRAPPER_BYTES:
            raise ValueError("linked native write wrapper bytes differ")
        if self.span(outw["file_offset"], 2) != b"\x66\xef":
            raise ValueError("OUTW label does not identify the 16-bit OUT prefix")
        evidence.update(wrapper={**wrapper, "bytes": WRAPPER_BYTES.hex()}, outw=outw)
        return evidence


def lua_string(value):
    # JSON quoting alone is not a Lua string encoder for arbitrary paths.
    value = str(value)
    if any(ord(character) < 32 or ord(character) > 126 for character in value):
        raise ValueError("this bounded harness requires printable ASCII paths")
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def project_text(kernel, cxx, toolchain, sources, profile, reboot=False, poweroff=None, negative=False):
    defines = ["OK_ARCH_TARGET_" + profile.upper()]
    if reboot:
        defines.append("OK_NATIVE_PC8042_REBOOT=1")
    if poweroff is not None:
        defines.append(POLICY + "=" + str(poweroff))
    lines = [
        'set_project("terminal-poweroff-hosted-controls-v1")',
        'set_languages("c++23")',
        'set_toolchains(' + lua_string(toolchain) + ')',
        'target("control")',
        '    set_kind(' + lua_string("object" if negative else "binary") + ')',
        '    set_default(true)',
        '    set_toolset("cxx", ' + lua_string(cxx) + ')',
        '    set_toolset("ld", ' + lua_string(cxx) + ')',
        '    set_targetdir("bin")',
        '    set_objectdir("objects")',
        '    set_symbols("debug")',
        '    set_strip("none")',
        '    set_optimize("none")',
        '    add_includedirs(' + lua_string(kernel / "include") + ', ' +
            lua_string(kernel / "include/ok/arch") + ')',
        '    add_defines(' + ', '.join(map(lua_string, defines)) + ')',
        '    add_cxxflags("-Wall", "-Wextra", "-Werror", "-mno-red-zone", "-fno-lto", {force=true})',
        '    add_ldflags("-fno-lto", {force=true})',
        '    add_files(' + ', '.join(map(lua_string, sources)) + ')',
        'target_end()',
    ]
    return "\n".join(lines) + "\n", defines


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel-root", type=Path, required=True)
    parser.add_argument("--policy-test", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--xmake", type=Path, required=True)
    parser.add_argument("--host-cxx", type=Path, required=True)
    parser.add_argument("--host-toolchain", choices=("gcc", "clang"), required=True)
    parser.add_argument("--command-timeout", type=int, default=60)
    parser.add_argument("--jobs", type=int, default=2)
    args = parser.parse_args()
    if sys.platform != "linux" or platform.machine().lower() not in ("x86_64", "amd64"):
        raise ValueError("this bounded hosted harness requires a Linux x86_64 host")
    if not 1 <= args.command_timeout <= 60 or not 1 <= args.jobs <= 4:
        raise ValueError("command timeout must be 1..60 seconds and jobs 1..4")
    if not args.kernel_root.is_absolute() or not args.output.is_absolute():
        raise ValueError("explicit absolute kernel and output paths required")
    kernel = args.kernel_root.resolve(strict=True)
    inputs = [exact_file(kernel / relative) for relative in TERMINAL_INPUTS]
    policy_test = exact_file(args.policy_test)
    tools = {"xmake": exact_file(args.xmake), "host_cxx": exact_file(args.host_cxx),
             "python": exact_file(Path(sys.executable))}
    for name in ("xmake", "host_cxx"):
        if not os.access(tools[name], os.X_OK):
            raise ValueError("executable tool required: " + str(tools[name]))
    runner = Path(__file__).resolve(strict=True)
    inputs.extend([policy_test, runner])
    # Quoted includes search the policy test's own directory before -I paths.
    # Admit a colocated draft header only if it is byte-identical to the kernel header.
    adjacent_header = policy_test.parent / "piix4_pm_poweroff.hpp"
    if adjacent_header.exists():
        adjacent_header = exact_file(adjacent_header)
        if sha(adjacent_header) != sha(kernel / "include/ok/arch/piix4_pm_poweroff.hpp"):
            raise ValueError("policy test's adjacent header differs from the integrated kernel header")
        inputs.append(adjacent_header)
    inputs = tuple(dict.fromkeys(inputs))
    output = args.output.resolve()
    if output == kernel or output in kernel.parents:
        raise ValueError("output must not contain the kernel input tree")
    output.mkdir(parents=True, exist_ok=False)
    (output / "home").mkdir()
    (output / "tmp").mkdir()
    (output / "logs").mkdir()
    environment = {
        "PATH": os.pathsep.join(dict.fromkeys([str(tools["host_cxx"].parent), str(tools["xmake"].parent),
                                               "/usr/bin", "/bin"])),
        "HOME": str(output / "home"), "TMPDIR": str(output / "tmp"),
        "LANG": "C", "LC_ALL": "C", "XMAKE_ROOT": "y",
    }
    record = {
        "schema": "terminal-poweroff-hosted-controls-v1", "status": "running",
        "scope": __doc__.strip(), "kernel_root": str(kernel), "policy_test": str(policy_test),
        "host": {"platform": sys.platform, "machine": platform.machine(), "python": sys.version},
        "host_toolchain": args.host_toolchain, "environment": environment,
        "source_sha256": {str(path): sha(path) for path in inputs},
        "tools": {name: {"path": str(path), "sha256": sha(path)} for name, path in tools.items()},
        "tool_pin_scope": "Explicit xmake, C++ driver and Python executable files; version output retained. "
                          "Not a complete pin of system headers, shared libraries or implicit subordinate tools.",
        "commands": [], "cases": [], "negative_controls": [], "generated_sha256": {},
        "privileged_execution": False, "cross_isa_execution": False, "guest_execution": False,
        "full_M0_M12": False,
    }

    def save():
        temporary = output / "result.json.tmp"
        temporary.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
        temporary.replace(output / "result.json")

    def run(argv, cwd, label, expected_failure=False):
        argv = list(map(str, argv))
        log = output / "logs" / (label + ".log")
        command = {"argv": argv, "cwd": str(cwd), "log": str(log.relative_to(output)),
                   "status": "starting", "expected_failure": expected_failure,
                   "timed_out": False, "natural_exit": False}
        record["commands"].append(command)
        save()
        started = time.monotonic()
        process = None
        with log.open("xb") as stream:
            try:
                process = subprocess.Popen(argv, cwd=cwd, env=environment, stdin=subprocess.DEVNULL,
                                           stdout=stream, stderr=subprocess.STDOUT, start_new_session=True)
                command["pid"] = process.pid
                command["returncode"] = process.wait(timeout=args.command_timeout)
                command["status"] = "exited"
                command["natural_exit"] = command["returncode"] >= 0
                command["post_exit_live_members"] = live_group_members(process.pid)
                if command["post_exit_live_members"]:
                    raise RuntimeError("command left live process-group children after direct exit")
                command["group_completion"] = "no-non-zombie-members-after-direct-exit"
            except BaseException as error:
                command.update(status="failed", error=repr(error),
                               timed_out=isinstance(error, subprocess.TimeoutExpired))
                if process is not None:
                    try:
                        retire_group(process, command)
                    except BaseException as retirement_error:
                        command["group_retirement_error"] = repr(retirement_error)
                        raise RuntimeError("command group retirement could not be established") from retirement_error
                raise
            finally:
                command["elapsed_seconds"] = time.monotonic() - started
                stream.flush()
                command["log_sha256"] = sha(log)
                save()
        if command["returncode"] < 0:
            raise RuntimeError("command ended by signal: " + label)
        if bool(command["returncode"]) != expected_failure:
            raise RuntimeError("unexpected command exit status: " + label)
        if log.stat().st_size > 16 * 1024 * 1024:
            raise RuntimeError("command log exceeds bounded inspection size: " + label)
        return log.read_bytes()

    def prepare(label, sources, profile, reboot=False, poweroff=None, negative=False):
        project = output / "cases" / label
        project.mkdir(parents=True, exist_ok=False)
        text, defines = project_text(kernel, tools["host_cxx"], args.host_toolchain, sources,
                                     profile, reboot, poweroff, negative)
        generated = project / "xmake.lua"
        generated.write_text(text, encoding="utf-8")
        record["generated_sha256"][str(generated.relative_to(output))] = sha(generated)
        save()
        run([tools["xmake"], "f", "-c", "-y", "--ccache=n", "--buildir=build"], project,
            "configure-" + label)
        return project, defines

    save()
    try:
        run([tools["xmake"], "--version"], output, "xmake-version")
        run([tools["host_cxx"], "--version"], output, "host-cxx-version")
        matrix = [(profile + "-default", profile, False, None) for profile in PROFILES]
        matrix.extend([
            ("x86_64-reboot-only", "x86_64", True, None),
            ("x86_64-poweroff-only", "x86_64", False, 1),
            ("x86_64-reboot-poweroff", "x86_64", True, 1),
        ])
        terminal_sources = [kernel / "src/arch/terminal_power.cpp", kernel / "tests/host/terminal_power_tests.cpp"]
        for label, profile, reboot, poweroff in matrix:
            project, defines = prepare(label, terminal_sources, profile, reboot, poweroff)
            run([tools["xmake"], "build", "-j" + str(args.jobs), "-v", "control"], project, "build-" + label)
            binary = exact_file(project / "bin/control")
            binary_sha = sha(binary)
            static = LinkedElf(binary).check_terminal(poweroff == 1)
            stdout = run([binary], project, "run-" + label)
            expected = ("TERMINAL_POWER_HOST_PASS profile=" + profile +
                        " values=256 returned-holds=4 pc8042=fake-ports scope=model-and-readonly-capability\n")
            if stdout != expected.encode("ascii"):
                raise RuntimeError("exact hosted terminal model output differs: " + label)
            if sha(binary) != binary_sha:
                raise RuntimeError("linked binary changed during hosted execution: " + label)
            record["cases"].append({
                "label": label, "profile_selection": profile, "defines": defines,
                "reboot_enabled": reboot, "poweroff_enabled": poweroff == 1,
                "binary": str(binary.relative_to(output)), "binary_sha256": binary_sha,
                "linked_static_inspection": static, "status": "passed-hosted-model-and-static-bytes",
            })
            save()
        label = "piix4-fake-policy"
        project, defines = prepare(label, [policy_test], "x86_64")
        run([tools["xmake"], "build", "-j" + str(args.jobs), "-v", "control"], project, "build-" + label)
        binary = exact_file(project / "bin/control")
        binary_sha = sha(binary)
        stdout = run([binary], project, "run-" + label)
        expected = b"PIIX4 PM hosted policy controls passed; no privileged I/O, guest execution, or hardware qualification\n"
        if stdout != expected or sha(binary) != binary_sha:
            raise RuntimeError("fake policy output or binary hash differs")
        record["policy_suite"] = {
            "label": label, "defines": defines, "binary": str(binary.relative_to(output)),
            "binary_sha256": binary_sha, "status": "passed-fake-ports-only",
        }
        save()
        for label, profile, value in (
            ("refuse-non-x86-poweroff", "aarch64", 1),
            ("refuse-poweroff-zero", "x86_64", 0),
            ("refuse-poweroff-two", "x86_64", 2),
        ):
            project, defines = prepare(label, [terminal_sources[0]], profile,
                                       poweroff=value, negative=True)
            stdout = run([tools["xmake"], "build", "-j1", "-v", "control"], project,
                         "build-" + label, expected_failure=True)
            if REFUSAL.encode("ascii") not in stdout:
                raise RuntimeError("negative compile did not report the intended refusal: " + label)
            record["negative_controls"].append({
                "label": label, "defines": defines, "required_diagnostic": REFUSAL,
                "status": "passed-compile-refusal", "binary_executed": False,
            })
            save()
        if len(record["cases"]) != 13 or len(record["negative_controls"]) != 3:
            raise RuntimeError("explicit hosted matrix is incomplete")
        for name, digest in record["source_sha256"].items():
            if sha(Path(name)) != digest:
                raise RuntimeError("pinned source changed during controls: " + name)
        for tool in record["tools"].values():
            if sha(Path(tool["path"])) != tool["sha256"]:
                raise RuntimeError("pinned tool changed during controls: " + tool["path"])
        for name, digest in record["generated_sha256"].items():
            if sha(output / name) != digest:
                raise RuntimeError("generated xmake project changed during controls: " + name)
        for row in [*record["cases"], record["policy_suite"]]:
            if sha(output / row["binary"]) != row["binary_sha256"]:
                raise RuntimeError("retained linked binary changed: " + row["label"])
        record["status"] = "passed-hosted-models-static-elf-and-compile-refusals"
    except BaseException as error:
        record.update(status="failed", error=repr(error))
        save()
        raise
    save()
    print("PASS 13 hosted terminal models, PIIX4 fake policy and 3 compile refusals; static ELF only, no native poweroff.")


if __name__ == "__main__":
    main()
