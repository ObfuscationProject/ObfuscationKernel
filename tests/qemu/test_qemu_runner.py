#!/usr/bin/env python3
"""Unit tests for the P0 QEMU validation harness."""

from __future__ import annotations

import contextlib
import importlib.util
import io
import tempfile
import unittest
from pathlib import Path
from unittest import mock


PROJECT_ROOT = Path(__file__).resolve().parents[2]
QEMU_TEST = PROJECT_ROOT / "scripts" / "qemu_test.py"

spec = importlib.util.spec_from_file_location("qemu_test", QEMU_TEST)
assert spec is not None and spec.loader is not None
qemu_test = importlib.util.module_from_spec(spec)
spec.loader.exec_module(qemu_test)


def make_output(arch: str, fields: dict[str, str] | None = None, include_display_text: bool = True) -> str:
    merged = {
        "arch": arch,
        "debug_test_points": "9",
        "fs": "1",
        "simplefs": "1",
        "ext4": "1",
        "user": "1",
        "display": "1",
        "gpu": "1",
        "input": "1",
        "posix": "1",
        "bus": "1",
        "usb": "1",
        "net": "1",
        "shell": "1",
        "modes": "1",
        "display_checksum": "1234",
    }
    if fields:
        for key, value in fields.items():
            if value == "":
                merged.pop(key, None)
            else:
                merged[key] = value
    pass_line = "OK_TEST_PASS " + " ".join(f"{key}={value}" for key, value in merged.items())
    lines = [
        "OK_MODE debug",
        "OK_DEBUG boot=complete",
        "OK_MODULES count=12 failed=0",
    ]
    if include_display_text:
        lines.append("OK_DISPLAY_TEXT booted")
    lines.append(pass_line)
    return "\n".join(lines) + "\n"


class QemuRunnerValidationTests(unittest.TestCase):
    def validate(self, arch: str, output: str, returncode: int = 0, debug_exit: bool = False) -> tuple[int, str, str]:
        stdout = io.StringIO()
        stderr = io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            code = qemu_test.validate_output(arch, output, returncode, debug_exit)
        return code, stdout.getvalue(), stderr.getvalue()

    def test_x86_64_requires_full_capability_coverage(self) -> None:
        output = make_output("x86_64")
        code, stdout, stderr = self.validate("x86_64", output)
        self.assertEqual(code, 0, stderr)
        self.assertIn("OK_QEMU_NET_TEST arch=x86_64", stdout)

    def test_missing_required_coverage_field_fails_clearly(self) -> None:
        output = make_output("x86_64", {"usb": ""})
        code, _, stderr = self.validate("x86_64", output)
        self.assertEqual(code, 8)
        self.assertIn("required coverage field: usb", stderr)

    def test_absent_capability_coverage_is_reported_as_skip(self) -> None:
        output = make_output("arm32", {"gpu": "", "input": "", "bus": "", "usb": ""})
        code, stdout, stderr = self.validate("arm32", output)
        self.assertEqual(code, 0, stderr)
        self.assertIn("OK_QEMU_SKIP arch=arm32 coverage=gpu reason=missing_capability", stdout)
        self.assertIn("OK_QEMU_SKIP arch=arm32 coverage=input reason=missing_capability", stdout)
        self.assertIn("OK_QEMU_SKIP arch=arm32 coverage=bus reason=missing_capability", stdout)
        self.assertIn("OK_QEMU_SKIP arch=arm32 coverage=usb reason=missing_capability", stdout)

    def test_wrong_arch_marker_fails(self) -> None:
        output = make_output("rv64")
        code, _, stderr = self.validate("x86_64", output)
        self.assertEqual(code, 6)
        self.assertIn("arch mismatch", stderr)

    def test_timeout_has_dedicated_reason(self) -> None:
        code, _, stderr = self.validate("x86_64", "", returncode=124)
        self.assertEqual(code, 12)
        self.assertIn("timed out before OK_TEST_PASS", stderr)

    def test_summary_records_coverage_status_and_roadmap_markers(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            kernel = Path(tmp) / "kernel.bin"
            kernel.write_bytes(b"ok")
            output = make_output("arm32", {"gpu": "", "input": "", "bus": "", "usb": ""})
            qemu_test.write_summary(kernel, "arm32", output, 0, 0)
            summary = (kernel.parent / "qemu-test-summary.txt").read_text(encoding="utf-8")
        self.assertIn("coverage.gpu=skip", summary)
        self.assertIn("skip.gpu=missing_capability", summary)
        self.assertIn("coverage.display=pass", summary)
        self.assertIn("marker.OK_MODULES=present", summary)

    def test_missing_qemu_executable_is_explicit(self) -> None:
        with mock.patch.object(qemu_test.shutil, "which", return_value=None):
            with self.assertRaises(qemu_test.QemuConfigurationError) as error:
                qemu_test.qemu_command("x86_64", Path("kernel.bin"), "none", True, Path("disk.img"))
        self.assertIn("QEMU executable missing: qemu-system-x86_64", str(error.exception))


if __name__ == "__main__":
    unittest.main()
