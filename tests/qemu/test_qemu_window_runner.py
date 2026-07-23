#!/usr/bin/env python3
"""Unit tests for the QEMU window validation harness."""

from __future__ import annotations

import importlib.util
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
QEMU_WINDOW_TEST = PROJECT_ROOT / "scripts" / "qemu_window_test.py"

spec = importlib.util.spec_from_file_location("qemu_window_test", QEMU_WINDOW_TEST)
assert spec is not None and spec.loader is not None
qemu_window_test = importlib.util.module_from_spec(spec)
spec.loader.exec_module(qemu_window_test)


def systoolchain_qemu(arch: str) -> Path:
    names = {
        "i386": "qemu-system-i386",
        "x86_64": "qemu-system-x86_64",
        "aarch64": "qemu-system-aarch64",
        "arm32": "qemu-system-arm",
        "rv64": "qemu-system-riscv64",
        "rv32": "qemu-system-riscv32",
        "loongarch64": "qemu-system-loongarch64",
        "mips": "qemu-system-mips",
        "mips64": "qemu-system-mips64",
        "ppc": "qemu-system-ppc",
    }
    return Path("/xmake/packages/s/systoolchain/0.1.0/bin") / names[arch]


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
        for arch in arches:
            with self.subTest(arch=arch):
                qemu = systoolchain_qemu(arch)
                command = qemu_window_test.qemu_command(
                    arch, qemu, Path("kernel.bin"), "none", Path("disk.img")
                )
                self.assertEqual(command[0], str(qemu))
                self.assertIn("-display", command)
                if arch == "mips64":
                    self.assertIn("MIPS64R2-generic", command)

    def test_malta_and_ppce500_profiles_do_not_claim_ramfb(self) -> None:
        for arch in ("mips", "mips64", "ppc"):
            with self.subTest(arch=arch):
                self.assertNotIn("ramfb", qemu_window_test.CAPABILITIES_BY_ARCH[arch])
                self.assertIn("framebuffer", qemu_window_test.CAPABILITIES_BY_ARCH[arch])

    def test_mips_window_command_disables_guest_vga_and_uses_serial_vc(self) -> None:
        command = qemu_window_test.qemu_command(
            "mips", systoolchain_qemu("mips"), Path("kernel.bin"), "sdl", Path("disk.img")
        )
        self.assertIn("vc:80Cx24C", command)
        self.assertIn("stdio", command)
        self.assertNotIn("ramfb", command)
        self.assertEqual(command[command.index("-vga") + 1], "none")

    def test_mips64_window_command_disables_guest_vga_and_uses_serial_vc(self) -> None:
        command = qemu_window_test.qemu_command(
            "mips64", systoolchain_qemu("mips64"), Path("kernel.bin"), "sdl", Path("disk.img")
        )
        self.assertIn("vc:80Cx24C", command)
        self.assertIn("stdio", command)
        self.assertNotIn("ramfb", command)
        self.assertEqual(command[command.index("-vga") + 1], "none")

    def test_ppc_window_command_uses_serial_vc(self) -> None:
        command = qemu_window_test.qemu_command(
            "ppc", systoolchain_qemu("ppc"), Path("kernel.bin"), "sdl", Path("disk.img")
        )
        self.assertIn("vc:80Cx24C", command)
        self.assertIn("stdio", command)
        self.assertNotIn("ramfb", command)


if __name__ == "__main__":
    unittest.main()
