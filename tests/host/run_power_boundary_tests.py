#!/usr/bin/env python3
"""Run the actual desktop loop against hosted stubs; never execute platform power."""

import argparse
from contextlib import nullcontext
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
EVIDENCE = None
COMMANDS = []


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def command(argv, cwd, label):
    result = subprocess.run(argv, cwd=cwd, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, check=False)
    record = {"argv": [str(value) for value in argv], "cwd": str(cwd),
              "returncode": result.returncode}
    if EVIDENCE is not None:
        log = EVIDENCE / (label + ".log")
        log.write_text(result.stdout)
        record.update(log=log.name, log_sha256=digest(log))
    COMMANDS.append(record)
    return result


def function_source(source, marker):
    start = source.index(marker)
    opening = source.index("{", start)
    depth = 1
    for position in range(opening + 1, len(source)):
        if source[position] == "{":
            depth += 1
        elif source[position] == "}":
            depth -= 1
            if depth == 0:
                return source[start:position + 1]
    raise ValueError("unterminated function: " + marker)


def desktop_loop_source(source):
    return function_source(source, "[[maybe_unused, noreturn]] void desktop_loop(bool show_shell)")


class PowerBoundaryTests(unittest.TestCase):
    def setUp(self):
        self.source = (ROOT / "src/core/kernel_main.cpp").read_text()
        self.loop = desktop_loop_source(self.source)

    def test_legacy_action_cannot_reach_platform(self):
        # Compile the product loop verbatim: valid and malformed enum values
        # cannot be consumed even if a caller bypasses request admission.
        template = (ROOT / "tests/host/power_boundary_tests.cpp.in").read_text()
        self.assertEqual(template.count("@DESKTOP_LOOP@"), 1)
        directory = (nullcontext(EVIDENCE / "fixture") if EVIDENCE is not None else
                     tempfile.TemporaryDirectory(prefix="ok-power-boundary-"))
        with directory as temporary:
            project = Path(temporary)
            project.mkdir(exist_ok=True)
            source = project / "desktop_loop.cpp"
            executable = project / "bin/desktop_loop"
            source.write_text(template.replace("@DESKTOP_LOOP@", self.loop))
            (project / "xmake.lua").write_text('''set_project("hosted-desktop-power-boundary")
set_languages("c++23")
set_optimize("fast")
target("desktop_loop")
    set_kind("binary")
    set_targetdir("bin")
    add_cxxflags("-Wall", "-Wextra", "-Werror", {force = true})
    add_files("desktop_loop.cpp")
target_end()
''')
            xmake = shutil.which("xmake")
            self.assertIsNotNone(xmake, "xmake is required for hosted fixture compilation")
            configured = command([xmake, "f", "-P", str(project), "-c", "-y", "--ccache=n"],
                                 project, "xmake-configure")
            self.assertEqual(configured.returncode, 0, configured.stdout)
            built = command([xmake, "build", "-P", str(project), "-v", "desktop_loop"],
                            project, "xmake-build")
            self.assertEqual(built.returncode, 0, built.stdout)
            run = command([str(executable)], project, "hosted-run")
            self.assertEqual(run.returncode, 0, run.stdout)
            self.assertIn("legacy=24 queued=24 passed; no platform execution claim", run.stdout)

    def test_reboot_hook_removed_but_diagnostic_exit_retained(self):
        self.assertNotIn("ok_platform_reboot", self.source)
        self.assertNotIn("ok_system_power_action", self.loop)
        self.assertIn('platform_write("OK_TEST_FAIL native-runtime=")', self.loop)
        self.assertIn("ok_platform_debug_exit(0x11u)", self.loop)
        self.assertIn("ok_platform_debug_exit(status.ok() ? 0x10u : 0x11u)", self.source)
        self.assertIn("namespace ok::detail", self.source)
        self.assertIn("NativeShutdownBoundary boundary", self.loop)
        self.assertEqual(self.loop.count("shutdown_boundary();"), 2)



if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, help="Retain actual commands, logs and hosted fixture")
    arguments, remaining = parser.parse_known_args()
    if arguments.output is not None:
        EVIDENCE = arguments.output.resolve()
        EVIDENCE.mkdir(parents=True, exist_ok=False)
    program = unittest.main(argv=[__file__, *remaining], exit=False)
    if EVIDENCE is not None:
        inputs = ("src/core/kernel_main.cpp", "src/core/kernel.cpp",
                  "tests/host/power_boundary_tests.cpp.in", "tests/host/run_power_boundary_tests.py")
        result = {"schema": 1, "status": "passed" if program.result.wasSuccessful() else "failed",
                  "scope": "Hosted xmake-built execution of actual desktop_loop plus a diagnostic-boundary source guard; no platform execution or clean shutdown claim.",
                  "source": str(ROOT), "source_sha256": {name: digest(ROOT / name) for name in inputs},
                  "tests_run": program.result.testsRun, "failures": len(program.result.failures),
                  "errors": len(program.result.errors), "commands": COMMANDS}
        fixture = EVIDENCE / "fixture"
        result["retained_fixture_sha256"] = {
            str(path.relative_to(fixture)): digest(path) for path in sorted(fixture.rglob("*"))
            if path.is_file() and ".xmake" not in path.relative_to(fixture).parts and
            "build" not in path.relative_to(fixture).parts}
        (EVIDENCE / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    raise SystemExit(0 if program.result.wasSuccessful() else 1)
