#!/usr/bin/env python3
"""Use xmake for ten hosted profile queries and returning-backend hold-loop checks."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import struct

ROOT = Path(__file__).resolve().parents[2]
PROFILES = ("i386", "x86_64", "aarch64", "arm32", "rv64", "rv32", "loongarch64", "mips", "mips64", "ppc")


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def inspect_hold_loop(path):
    """Read linked ELF bytes without executing CLI/HLT or privileged port I/O."""
    data = path.read_bytes()
    if data[:6] != b'\x7fELF\x02\x01': raise ValueError('ELF64 little-endian host test required')
    offset = struct.unpack_from('<Q', data, 40)[0]
    width, count = struct.unpack_from('<HH', data, 58)
    sections = [struct.unpack_from('<IIQQQQIIQQ', data, offset + width * i) for i in range(count)]
    matches = []
    for section in sections:
        if section[1] != 2: continue
        strings = sections[section[6]]
        names = data[strings[4]:strings[4] + strings[5]]
        for at in range(section[4], section[4] + section[5], section[9]):
            name, info, _, index, address, size = struct.unpack_from('<IBBHQQ', data, at)
            if names[name:names.find(b'\0', name)] != b'ok_arch_terminal_hold': continue
            owner = sections[index]
            start = owner[4] + address - owner[3]
            if info & 15 != 2 or owner[2] & 6 != 6 or size != 4 or data[start:start + size] != b'\xfa\xf4\xeb\xfc':
                raise ValueError('terminal hold symbol/allocated instruction bytes differ')
            matches.append({'symbol': 'ok_arch_terminal_hold', 'address': address, 'bytes': 'faf4ebfc', 'size': size})
    if len(matches) != 1: raise ValueError('one exact terminal hold symbol required')
    return matches[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    project = out / "project"
    project.mkdir()
    record = {"schema": 1, "status": "running", "scope": __doc__ +
              " No cross-ISA, privileged instruction, guest or clean storage execution claim.",
              "commands": [], "rows": []}

    def save():
        (out / "result.json").write_text(json.dumps(record, indent=2) + "\n")

    def run(argv, label):
        result = subprocess.run(list(map(str, argv)), cwd=project, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, check=False)
        log = out / (label + ".log")
        log.write_bytes(result.stdout)
        record["commands"].append({"argv": list(map(str, argv)), "returncode": result.returncode,
                                   "log": log.name, "log_sha256": sha(log)})
        save()
        if result.returncode: raise RuntimeError(label + " failed")
        return result.stdout.decode()

    try:
        inputs = [ROOT / "src/arch/terminal_power.cpp", ROOT / "tests/host/terminal_power_tests.cpp"]
        inputs += list((ROOT / "include/ok/arch").glob("*.hpp"))
        inputs += [ROOT / "include/ok/core/types.hpp", ROOT / "include/ok/core/power.hpp"]
        record["input_sha256"] = {str(path.relative_to(ROOT)): sha(path) for path in inputs}
        record["runner_sha256"] = sha(Path(__file__))
        lua = ['set_project("terminal-power-hosted-profile-tests")', 'set_languages("c++23")']
        profiles = [(profile, profile, False) for profile in PROFILES] + [('x86_64-enabled', 'x86_64', True)]
        for target, profile, enabled in profiles:
            lua += ['target("host-' + target + '")', '    set_kind("binary")',
                    '    set_targetdir("bin")', '    add_includedirs(' + json.dumps(str(ROOT / "include")) + ')',
                    '    add_defines("OK_ARCH_TARGET_' + profile.upper() + '")',
                    *(['    add_defines("OK_NATIVE_PC8042_REBOOT=1")'] if enabled else []),
                    '    add_cxxflags("-Wall", "-Wextra", "-Werror", {force=true})',
                    '    add_files(' + ', '.join(json.dumps(str(path)) for path in inputs[:2]) + ')', 'target_end()']
        (project / "xmake.lua").write_text('\n'.join(lua) + '\n')
        record["generated_xmake_sha256"] = sha(project / "xmake.lua")
        run(["xmake", "f", "-c", "-y", "--ccache=n"], "configure")
        run(["xmake", "build", "-j4", "-v"], "build")
        for target, profile, enabled in profiles:
            binary = project / "bin" / ("host-" + target)
            output = run([binary], "run-" + target)
            if "TERMINAL_POWER_HOST_PASS profile=" + profile not in output:
                raise RuntimeError("profile marker mismatch")
            record["rows"].append({"profile": profile, "reboot_enabled": enabled, "binary_sha256": sha(binary),
                                   "status": "passed", "output": output.strip()})
            if profile == 'x86_64': record['rows'][-1]['linked_hold_loop'] = inspect_hold_loop(binary)
        for name, digest in record["input_sha256"].items():
            if sha(ROOT / name) != digest: raise RuntimeError("input changed during tests")
        record["status"] = "passed"
    except BaseException as error:
        record.update(status="failed", error=repr(error))
        save()
        raise
    save()
    print("PASS ten hosted terminal-power profiles; no privileged/native shutdown execution.")


if __name__ == "__main__":
    main()
