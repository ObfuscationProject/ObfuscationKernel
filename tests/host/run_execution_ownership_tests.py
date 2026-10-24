#!/usr/bin/env python3
"""Hosted actual x86_64 ownership snapshot and unsupported stub checks through xmake."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    out = parser.parse_args().output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    project = out / "project"
    project.mkdir()
    record = {"schema": 1, "status": "running", "scope": __doc__ +
              " Mocked initialized flag; no native entry, hardware isolation or all-ISA execution claim.",
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
        names = ["include/ok/arch/execution.hpp", "src/arch/execution_stub.cpp",
                 "src/arch/mips_common/execution_impl.hpp", "tests/host/execution_ownership_tests.cpp"]
        names += ["src/arch/" + profile + "/execution.cpp" for profile in
                  ("i386", "x86_64", "aarch64", "arm32", "rv64", "rv32", "loongarch64", "ppc")]
        record["source_sha256"] = {name: sha(ROOT / name) for name in names}
        record["runner_sha256"] = sha(Path(__file__))
        for name in names[1:]:
            if name.endswith("execution.cpp") or name.endswith("execution_impl.hpp") or name.endswith("execution_stub.cpp"):
                source = (ROOT / name).read_text()
                if source.count("OwnershipReport ownership_report() noexcept") != 1:
                    raise RuntimeError("missing/duplicate concrete ownership report: " + name)
        lua = ['set_project("execution-owner-hosted-tests")', 'set_languages("c++23")']
        for backend in ("x86_64", "stub"):
            lua += ['target("owner-' + backend + '")', '    set_kind("binary")',
                    '    set_targetdir("bin")', '    add_includedirs(' + json.dumps(str(ROOT / "include")) + ')',
                    '    add_defines("OK_ARCH_TARGET_X86_64")',
                    '    add_cxxflags("-Wall", "-Wextra", "-Werror", "-ffunction-sections", "-fdata-sections", {force=true})',
                    '    add_ldflags("-Wl,--gc-sections", {force=true})',
                    '    add_files(' + json.dumps(str(ROOT / "tests/host/execution_ownership_tests.cpp")) + ')']
            if backend == "stub": lua += ['    add_defines("OK_TEST_EXECUTION_STUB")']
            lua += ['target_end()']
        (project / "xmake.lua").write_text('\n'.join(lua) + '\n')
        record["generated_xmake_sha256"] = sha(project / "xmake.lua")
        run(["xmake", "f", "-c", "-y", "--ccache=n"], "configure")
        run(["xmake", "build", "-j2", "-v"], "build")
        for backend in ("x86_64", "stub"):
            binary = project / "bin" / ("owner-" + backend)
            output = run([binary], "run-" + backend)
            if "EXECUTION_OWNERSHIP_HOST_PASS" not in output: raise RuntimeError("missing pass marker")
            record["rows"].append({"backend": backend, "binary_sha256": sha(binary), "output": output.strip(), "status": "passed"})
        for name, digest in record["source_sha256"].items():
            if sha(ROOT / name) != digest: raise RuntimeError("source changed during host checks")
        record["status"] = "passed"
    except BaseException as error:
        record.update(status="failed", error=repr(error))
        save()
        raise
    save()
    print("PASS actual x86_64 backend ownership host bookkeeping and unsupported stub; no native execution.")


if __name__ == "__main__":
    main()
