#!/usr/bin/env python3
"""Unit tests for the QEMU window validation harness."""

from __future__ import annotations

import importlib.util
import unittest
from pathlib import Path
from unittest import mock


PROJECT_ROOT = Path(__file__).resolve().parents[2]
QEMU_WINDOW_TEST = PROJECT_ROOT / "scripts" / "qemu_window_test.py"

spec = importlib.util.spec_from_file_location("qemu_window_test", QEMU_WINDOW_TEST)
assert spec is not None and spec.loader is not None
qemu_window_test = importlib.util.module_from_spec(spec)
spec.loader.exec_module(qemu_window_test)


def make_output(arch: str, fields: dict[str, str] | None = None) -> str:
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
        "gui": "1",
        "display_checksum": "1234",
    }
    if fields:
        for key, value in fields.items():
            if value == "":
                merged.pop(key, None)
            else:
                merged[key] = value
    pass_line = "OK_TEST_PASS " + " ".join(f"{key}={value}" for key, value in merged.items())
    return "\n".join(("OK_MODE debug", "OK_DEBUG boot=complete", pass_line)) + "\n"


class QemuWindowRunnerTests(unittest.TestCase):
    def test_window_validator_requires_full_coverage_for_new_boot_targets(self) -> None:
        output = make_output("mips", {"bus": ""})
        ok, detail = qemu_window_test.validate_output("mips", output)
        self.assertFalse(ok)
        self.assertIn("bus did not pass", detail)

    def test_window_validator_keeps_unknown_arch_capabilities_optional(self) -> None:
        output = make_output("profile-only", {"display": "", "gpu": "", "input": "", "bus": "", "usb": ""})
        ok, detail = qemu_window_test.validate_output("profile-only", output)
        self.assertTrue(ok, detail)

    def test_window_validator_requires_gui_base_coverage(self) -> None:
        output = make_output("x86_64", {"gui": ""})
        ok, detail = qemu_window_test.validate_output("x86_64", output)
        self.assertFalse(ok)
        self.assertIn("gui did not pass", detail)

    def test_qemu_window_command_covers_every_supported_architecture(self) -> None:
        arches = (
            "i386", "x86_64", "aarch64", "arm32", "rv64", "rv32", "loongarch64", "mips", "mips64", "ppc",
        )
        with mock.patch.object(qemu_window_test.shutil, "which", side_effect=lambda qemu: f"/usr/bin/{qemu}"):
            for arch in arches:
                with self.subTest(arch=arch):
                    command = qemu_window_test.qemu_command(arch, Path("kernel.bin"), "none", Path("disk.img"))
                    self.assertTrue(command[0].startswith("/usr/bin/qemu-system-"))
                    self.assertIn("-display", command)
                    if arch == "mips64":
                        self.assertIn("MIPS64R2-generic", command)

    def test_mips_window_command_disables_guest_vga_and_uses_serial_vc(self) -> None:
        with mock.patch.object(qemu_window_test.shutil, "which", return_value="/usr/bin/qemu-system-mips"):
            command = qemu_window_test.qemu_command("mips", Path("kernel.bin"), "gtk", Path("disk.img"))
        self.assertIn("vc:80Cx24C", command)
        self.assertIn("stdio", command)
        self.assertEqual(command[command.index("-vga") + 1], "none")

    def test_ppc_window_command_uses_serial_vc(self) -> None:
        with mock.patch.object(qemu_window_test.shutil, "which", return_value="/usr/bin/qemu-system-ppc"):
            command = qemu_window_test.qemu_command("ppc", Path("kernel.bin"), "gtk", Path("disk.img"))
        self.assertIn("vc:80Cx24C", command)
        self.assertIn("stdio", command)


if __name__ == "__main__":
    unittest.main()
