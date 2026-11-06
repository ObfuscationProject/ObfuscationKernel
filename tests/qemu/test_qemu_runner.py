#!/usr/bin/env python3
"""Unit tests for the baseline QEMU validation harness."""

from __future__ import annotations

import contextlib
import importlib.util
import io
import sys
import tempfile
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
QEMU_TEST = PROJECT_ROOT / "scripts" / "qemu_test.py"

spec = importlib.util.spec_from_file_location("qemu_test", QEMU_TEST)
assert spec is not None and spec.loader is not None
qemu_test = importlib.util.module_from_spec(spec)
spec.loader.exec_module(qemu_test)


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


# Frozen copy of the builder before the -smp lane; default commands must not drift from it.
def single_cpu_qemu_command(arch: str, qemu: Path, kernel: Path, display: str, debug_exit: bool, disk: Path) -> list[str]:
    if arch not in qemu_test.BOOTABLE_QEMU_ARCHES:
        raise qemu_test.QemuConfigurationError(f"qemu-system boot is not implemented for {arch} yet")
    qemu_path = str(qemu)

    if arch in ("aarch64", "arm32"):
        command = [
            qemu_path,
            "-M",
            "virt",
            "-cpu",
            "cortex-a57" if arch == "aarch64" else "cortex-a15",
            "-m",
            "256M",
            "-kernel",
            str(kernel),
            "-monitor",
            "none",
            "-no-reboot",
            "-display",
            display,
        ]
        command += qemu_test.serial_args(arch, display)
        command += qemu_test.ramfb_args()
        command += qemu_test.virtio_mmio_input_args()
        command += qemu_test.virtio_disk_args(disk)
        return command
    if arch in ("rv64", "rv32"):
        command = [
            qemu_path,
            "-M",
            "virt",
            "-m",
            "256M",
            "-bios",
            "none",
            "-kernel",
            str(kernel),
            "-monitor",
            "none",
            "-no-reboot",
            "-display",
            display,
        ]
        command += qemu_test.serial_args(arch, display)
        command += qemu_test.ramfb_args()
        command += qemu_test.virtio_mmio_input_args()
        command += qemu_test.virtio_disk_args(disk)
        return command
    if arch == "loongarch64":
        command = [
            qemu_path,
            "-M",
            "virt",
            "-m",
            "2G",
            "-kernel",
            str(kernel),
            "-monitor",
            "none",
            "-no-reboot",
            "-display",
            display,
        ]
        command += qemu_test.serial_args(arch, display)
        command += qemu_test.ramfb_args()
        command += qemu_test.virtio_pci_input_args()
        command += qemu_test.virtio_disk_args(disk)
        return command
    if arch in ("mips", "mips64"):
        command = [
            qemu_path,
            "-M",
            "malta",
        ]
        if arch == "mips64":
            command += ["-cpu", "MIPS64R2-generic"]
        command += [
            "-vga",
            "none",
            "-m",
            "256M",
            "-kernel",
            str(kernel),
            "-monitor",
            "none",
            "-no-reboot",
            "-display",
            display,
        ]
        command += qemu_test.serial_args(arch, display)
        command += qemu_test.virtio_disk_args(disk)
        return command
    if arch == "ppc":
        command = [
            qemu_path,
            "-M",
            "ppce500",
            "-m",
            "256M",
            "-kernel",
            str(kernel),
            "-monitor",
            "none",
            "-no-reboot",
            "-display",
            display,
        ]
        command += qemu_test.serial_args(arch, display)
        command += qemu_test.virtio_disk_args(disk)
        return command

    command = [
        qemu_path,
        "-drive",
        f"file={kernel},format=raw,if=ide",
        "-boot",
        "c",
        "-serial",
        "stdio",
        "-monitor",
        "none",
        "-no-reboot",
        "-vga",
        "none",
        "-display",
        display,
    ]
    command += qemu_test.ramfb_args()
    command += qemu_test.virtio_disk_args(disk)
    if debug_exit:
        command += ["-device", "isa-debug-exit,iobase=0xf4,iosize=0x04"]
    return command


def make_smp_output(arch: str, smp_line: str | None) -> str:
    output = make_output(arch) + "OK_SMP scope=model topology=pass per_cpu=pass ap_start=unsupported\n"
    return output + (smp_line + "\n" if smp_line else "")

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
    def test_marker_in_single_burst_is_consumed_before_child_exits(self) -> None:
        result = qemu_test.run_until_marker([sys.executable, "-c",
            "import os,time;os.write(1,b'boot line\\nOK_TEST_PASS arch=arm32\\n');time.sleep(5)"], 0.3)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("OK_TEST_PASS arch=arm32\n", result.stdout)

    def test_partial_marker_does_not_block_timeout_or_count_as_success(self) -> None:
        result = qemu_test.run_until_marker([sys.executable, "-c",
            "import os,time;os.write(1,b'OK_TEST_PASS arch=arm32');time.sleep(5)"], 0.2)
        self.assertEqual(result.returncode, 124)
        self.assertEqual(result.stdout, "OK_TEST_PASS arch=arm32")

    def test_split_fail_marker_is_reassembled(self) -> None:
        result = qemu_test.run_until_marker([sys.executable, "-c",
            "import os,time;os.write(1,b'OK_TEST_FA');time.sleep(.03);"
            "os.write(1,b'IL code=11\\n');time.sleep(5)"], 0.3)
        self.assertEqual(result.returncode, 0)
        self.assertIn("OK_TEST_FAIL code=11\n", result.stdout)

    def test_stderr_pressure_does_not_block_stdout_marker(self) -> None:
        result = qemu_test.run_until_marker([sys.executable, "-c",
            "import os,time;os.write(2,b'x'*131072);"
            "os.write(1,b'OK_TEST_PASS arch=arm32\\n');time.sleep(5)"], 0.5)
        self.assertEqual(result.returncode, 0, result.stderr[-200:])
        self.assertIn("OK_TEST_PASS arch=arm32\n", result.stdout)

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

    def test_bootable_arches_require_the_same_capability_coverage(self) -> None:
        expected = qemu_test.required_coverage_fields("x86_64")
        for arch in qemu_test.BOOTABLE_QEMU_ARCHES:
            with self.subTest(arch=arch):
                self.assertEqual(qemu_test.required_coverage_fields(arch), expected)

    def test_malta_and_ppce500_profiles_do_not_claim_ramfb(self) -> None:
        for arch in ("mips", "mips64", "ppc"):
            with self.subTest(arch=arch):
                self.assertNotIn("ramfb", qemu_test.CAPABILITIES_BY_ARCH[arch])
                self.assertIn("framebuffer", qemu_test.CAPABILITIES_BY_ARCH[arch])

    def test_missing_required_coverage_field_fails_clearly(self) -> None:
        output = make_output("arm32", {"usb": ""})
        code, _, stderr = self.validate("arm32", output)
        self.assertEqual(code, 8)
        self.assertIn("required coverage field: usb", stderr)

    def test_absent_capability_coverage_is_reported_as_skip(self) -> None:
        output = make_output(
            "profile-only",
            {"display": "", "gpu": "", "input": "", "bus": "", "usb": "", "net": ""},
            include_display_text=False,
        )
        code, stdout, stderr = self.validate("profile-only", output)
        self.assertEqual(code, 0, stderr)
        self.assertIn("OK_QEMU_SKIP arch=profile-only coverage=gpu reason=missing_capability", stdout)
        self.assertIn("OK_QEMU_SKIP arch=profile-only coverage=input reason=missing_capability", stdout)
        self.assertIn("OK_QEMU_SKIP arch=profile-only coverage=bus reason=missing_capability", stdout)
        self.assertIn("OK_QEMU_SKIP arch=profile-only coverage=usb reason=missing_capability", stdout)

    def test_wrong_arch_marker_fails(self) -> None:
        output = make_output("rv64")
        code, _, stderr = self.validate("x86_64", output)
        self.assertEqual(code, 6)
        self.assertIn("arch mismatch", stderr)

    def test_timeout_has_dedicated_reason(self) -> None:
        code, _, stderr = self.validate("x86_64", "", returncode=124)
        self.assertEqual(code, 12)
        self.assertIn("timed out before OK_TEST_PASS", stderr)

    def test_kernel_test_fail_has_dedicated_reason_before_debug_exit_code(self) -> None:
        output = "\n".join(
            [
                "OK_MODE debug",
                "OK_DEBUG boot=complete",
                "OK_TEST_FAIL code=11 message=kernel scheduler tick did not dispatch work on secondary CPUs",
            ]
        )
        code, _, stderr = self.validate("i386", output, returncode=35, debug_exit=True)
        self.assertEqual(code, 13)
        self.assertIn("OK_TEST_FAIL code=11", stderr)

    def test_summary_records_coverage_status_and_roadmap_markers(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            kernel = Path(tmp) / "kernel.bin"
            kernel.write_bytes(b"ok")
            output = make_output(
                "profile-only",
                {"display": "", "gpu": "", "input": "", "bus": "", "usb": "", "net": ""},
                include_display_text=False,
            )
            qemu_test.write_summary(kernel, "profile-only", output, 0, 0)
            summary = (kernel.parent / "qemu-test-summary.txt").read_text(encoding="utf-8")
        self.assertIn("coverage.gpu=skip", summary)
        self.assertIn("skip.gpu=missing_capability", summary)
        self.assertIn("coverage.display=skip", summary)
        self.assertIn("marker.OK_MODULES=present", summary)

    def test_summary_preserves_model_scope_without_claiming_hardware_execution(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            kernel = Path(tmp) / "kernel.bin"
            kernel.write_bytes(b"ok")
            output = make_output("x86_64") + "OK_SMP scope=model topology=pass\n"
            qemu_test.write_summary(kernel, "x86_64", output, 0, 0)
            summary = (kernel.parent / "qemu-test-summary.txt").read_text(encoding="utf-8")
        self.assertIn("scope.OK_SMP=model", summary)
        self.assertIn("scope.OK_MODULES=unspecified", summary)
        self.assertIn("scope.OK_IRQ=absent", summary)

    def validate_smp(self, arch: str, output: str, cpus: int) -> tuple[int, str, str]:
        stdout = io.StringIO()
        stderr = io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            code = qemu_test.validate_output(arch, output, 0, False, cpus)
        return code, stdout.getvalue(), stderr.getvalue()

    def test_single_cpu_run_ignores_missing_hardware_smp_marker(self) -> None:
        for arch in qemu_test.BOOTABLE_QEMU_ARCHES:
            with self.subTest(arch=arch):
                code, stdout, stderr = self.validate_smp(arch, make_smp_output(arch, None), 1)
                self.assertEqual(code, 0, stderr)
                self.assertNotIn("OK_QEMU_SMP_TEST", stdout)

    def test_smp_profiles_require_every_cpu_online_with_ipi(self) -> None:
        for arch in qemu_test.SMP_QEMU_ARCHES:
            with self.subTest(arch=arch):
                line = "OK_SMP scope=hardware cpus=4 online=4 ap_start=psci ipi=pass"
                if arch in qemu_test.TSC_SYNC_QEMU_ARCHES:
                    line += " tsc=sync"
                code, stdout, stderr = self.validate_smp(arch, make_smp_output(arch, line), 4)
                self.assertEqual(code, 0, stderr)
                self.assertIn(f"OK_QEMU_SMP_TEST arch={arch} smp=4 cpus=4 online=4", stdout)

    def test_x86_64_smp_report_names_its_tsc_synchronization(self) -> None:
        self.assertEqual(qemu_test.TSC_SYNC_QEMU_ARCHES, ("x86_64",))
        base = "OK_SMP scope=hardware cpus=2 online=2 ap_start=sipi ipi=pass"
        for result in ("sync", "adjusted", "offset", "unsynced"):
            with self.subTest(result=result):
                code, stdout, stderr = self.validate_smp("x86_64", make_smp_output("x86_64", f"{base} tsc={result}"), 2)
                self.assertEqual(code, 0, stderr)
                self.assertIn(f"OK_QEMU_SMP_TEST arch=x86_64 smp=2 cpus=2 online=2 ap_start=sipi ipi=pass tsc={result}",
                              stdout)
        for line in (base, f"{base} tsc=", f"{base} tsc=skewed"):
            with self.subTest(line=line):
                code, stdout, stderr = self.validate_smp("x86_64", make_smp_output("x86_64", line), 2)
                self.assertEqual(code, 14)
                self.assertIn("tsc=", stderr)
                self.assertNotIn("OK_QEMU_SMP_TEST", stdout)

    def test_tsc_field_is_optional_on_shared_counter_profiles(self) -> None:
        for arch in ("aarch64", "rv64"):
            line = "OK_SMP scope=hardware cpus=2 online=2 ap_start=psci ipi=pass"
            with self.subTest(arch=arch):
                code, stdout, stderr = self.validate_smp(arch, make_smp_output(arch, line), 2)
                self.assertEqual(code, 0, stderr)
                self.assertNotIn("tsc=", stdout)
                code, stdout, stderr = self.validate_smp(arch, make_smp_output(arch, line + " tsc=sync"), 2)
                self.assertEqual(code, 0, stderr)
                self.assertIn("tsc=sync", stdout)
                code, _, _ = self.validate_smp(arch, make_smp_output(arch, line + " tsc=bogus"), 2)
                self.assertEqual(code, 14)

    def test_smp_profiles_reject_partial_bringup_or_failed_ipi(self) -> None:
        rejected = (
            "OK_SMP scope=hardware cpus=2 online=1 ap_start=sipi ipi=pass",
            "OK_SMP scope=hardware cpus=1 online=1 ap_start=sipi ipi=pass",
            "OK_SMP scope=hardware cpus=2 online=2 ap_start=sipi ipi=fail",
            "OK_SMP scope=hardware cpus=2 online=2 ap_start=sipi",
            "OK_SMP scope=model cpus=2 online=2 ap_start=sipi ipi=pass",
            "OK_SMP scope=hardware cpus=1 online=1 ap_start=unsupported ipi=unsupported reason=no_ap_start",
        )
        for arch in qemu_test.SMP_QEMU_ARCHES:
            for line in rejected:
                with self.subTest(arch=arch, line=line):
                    code, stdout, stderr = self.validate_smp(arch, make_smp_output(arch, line), 2)
                    self.assertEqual(code, 14)
                    self.assertIn("-smp 2", stderr)
                    self.assertNotIn("OK_QEMU_SMP_TEST", stdout)

    def test_smp_run_without_hardware_marker_names_the_marker(self) -> None:
        code, _, stderr = self.validate_smp("x86_64", make_smp_output("x86_64", None), 2)
        self.assertEqual(code, 14)
        self.assertIn("did not report OK_SMP scope=hardware for -smp 2", stderr)

    def test_up_profiles_must_report_up_honestly(self) -> None:
        up_arches = [arch for arch in qemu_test.BOOTABLE_QEMU_ARCHES if arch not in qemu_test.SMP_QEMU_ARCHES]
        self.assertEqual(len(up_arches), 7)
        honest = "OK_SMP scope=hardware cpus=2 online=1 ap_start=unsupported ipi=unsupported reason=no_ap_start"
        claimed = (
            "OK_SMP scope=hardware cpus=2 online=2 ap_start=psci ipi=pass",
            "OK_SMP scope=hardware cpus=2 online=1 ap_start=sbi ipi=unsupported reason=no_ap_start",
            "OK_SMP scope=model cpus=1 online=1 ap_start=unsupported ipi=unsupported reason=no_ap_start",
        )
        for arch in up_arches:
            with self.subTest(arch=arch):
                code, stdout, stderr = self.validate_smp(arch, make_smp_output(arch, honest), 2)
                self.assertEqual(code, 0, stderr)
                self.assertIn(f"OK_QEMU_SMP_TEST arch={arch} smp=2", stdout)
                self.assertIn("reason=no_ap_start", stdout)
            for line in claimed:
                with self.subTest(arch=arch, line=line):
                    code, _, _ = self.validate_smp(arch, make_smp_output(arch, line), 2)
                    self.assertEqual(code, 14)

    def test_summary_records_cpu_count_and_both_smp_scopes(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            kernel = Path(tmp) / "kernel.bin"
            kernel.write_bytes(b"ok")
            output = make_smp_output("aarch64", "OK_SMP scope=hardware cpus=2 online=2 ap_start=psci ipi=pass")
            qemu_test.write_summary(kernel, "aarch64", output, 0, 0, 2)
            summary = (kernel.parent / "qemu-test-summary.txt").read_text(encoding="utf-8").splitlines()
        self.assertIn("smp.cpus=2", summary)
        self.assertIn("marker.OK_SMP=present", summary)
        self.assertIn("scopes.OK_SMP=model,hardware", summary)
        self.assertIn("marker.OK_SMP_HARDWARE=present", summary)
        self.assertIn("smp.hardware.online=2", summary)
        self.assertIn("smp.hardware.ap_start=psci", summary)

    def test_summary_defaults_to_one_cpu_without_hardware_smp(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            kernel = Path(tmp) / "kernel.bin"
            kernel.write_bytes(b"ok")
            qemu_test.write_summary(kernel, "x86_64", make_smp_output("x86_64", None), 0, 0)
            summary = (kernel.parent / "qemu-test-summary.txt").read_text(encoding="utf-8").splitlines()
        self.assertIn("smp.cpus=1", summary)
        self.assertIn("scope.OK_SMP=model", summary)
        self.assertIn("scopes.OK_SMP=model", summary)
        self.assertIn("scopes.OK_IRQ=absent", summary)
        self.assertIn("marker.OK_SMP_HARDWARE=missing", summary)
        self.assertFalse(any(line.startswith("smp.hardware.") for line in summary))

    def test_default_qemu_command_matches_single_cpu_builder(self) -> None:
        for arch in qemu_test.BOOTABLE_QEMU_ARCHES:
            for display in ("none", "sdl"):
                for debug_exit in (False, True):
                    with self.subTest(arch=arch, display=display, debug_exit=debug_exit):
                        args = (arch, systoolchain_qemu(arch), Path("kernel.bin"), display, debug_exit,
                                Path("disk.img"))
                        expected = single_cpu_qemu_command(*args)
                        self.assertEqual(qemu_test.qemu_command(*args), expected)
                        self.assertEqual(qemu_test.qemu_command(*args, 1), expected)
                        self.assertNotIn("-smp", expected)

    def test_qemu_command_appends_smp_only_above_one_cpu(self) -> None:
        for arch in qemu_test.BOOTABLE_QEMU_ARCHES:
            with self.subTest(arch=arch):
                args = (arch, systoolchain_qemu(arch), Path("kernel.bin"), "none", arch in ("i386", "x86_64"),
                        Path("disk.img"))
                command = qemu_test.qemu_command(*args, 4)
                self.assertEqual(command, single_cpu_qemu_command(*args) + ["-smp", "4"])

    def test_qemu_command_rejects_unsupported_cpu_counts(self) -> None:
        for cpus in (0, -1, qemu_test.MAX_QEMU_CPUS + 1):
            with self.subTest(cpus=cpus), self.assertRaises(qemu_test.QemuConfigurationError):
                qemu_test.qemu_command("x86_64", systoolchain_qemu("x86_64"), Path("kernel.bin"), "none", True,
                                       Path("disk.img"), cpus)

    def test_smp_cli_option_accepts_one_through_eight(self) -> None:
        self.assertEqual(qemu_test.smp_cpu_count("1"), 1)
        self.assertEqual(qemu_test.smp_cpu_count("8"), 8)
        for value in ("0", "9", "two"):
            with self.subTest(value=value), self.assertRaises(qemu_test.argparse.ArgumentTypeError):
                qemu_test.smp_cpu_count(value)

    def test_qemu_command_uses_explicit_systoolchain_executable(self) -> None:
        qemu = systoolchain_qemu("x86_64")
        command = qemu_test.qemu_command(
            "x86_64", qemu, Path("kernel.bin"), "none", True, Path("disk.img")
        )
        self.assertEqual(command[0], str(qemu))

    def test_qemu_command_attaches_real_virtio_disk_image(self) -> None:
        command = qemu_test.qemu_command(
            "x86_64", systoolchain_qemu("x86_64"), Path("kernel.bin"), "none", True, Path("disk.img")
        )
        self.assertIn("-drive", command)
        self.assertIn("file=disk.img,format=raw,if=none,id=fsdisk", command)
        self.assertIn("-device", command)
        self.assertIn("virtio-blk-pci,drive=fsdisk", command)
        self.assertIn("ramfb", command)

    def test_qemu_command_attaches_ramfb_and_input_for_virt_targets(self) -> None:
        command = qemu_test.qemu_command(
            "arm32", systoolchain_qemu("arm32"), Path("kernel.bin"), "none", False, Path("disk.img")
        )
        self.assertIn("ramfb", command)
        self.assertIn("virtio-keyboard-device", command)
        self.assertIn("virtio-mouse-device", command)

    def test_qemu_command_boots_new_system_targets_with_kernel_loader(self) -> None:
        arches = ("loongarch64", "mips", "mips64", "ppc")
        for arch in arches:
            with self.subTest(arch=arch):
                command = qemu_test.qemu_command(
                    arch, systoolchain_qemu(arch), Path("kernel.bin"), "none", False, Path("disk.img")
                )
                self.assertIn("-kernel", command)
                self.assertIn("kernel.bin", command)
                self.assertIn("virtio-blk-pci,drive=fsdisk", command)
                if arch == "mips64":
                    self.assertIn("MIPS64R2-generic", command)

    def test_mips_window_display_uses_serial_console_instead_of_guest_vga(self) -> None:
        command = qemu_test.qemu_command(
            "mips", systoolchain_qemu("mips"), Path("kernel.bin"), "sdl", False, Path("disk.img")
        )
        self.assertIn("vc:80Cx24C", command)
        self.assertIn("stdio", command)
        self.assertNotIn("ramfb", command)
        self.assertEqual(command[command.index("-vga") + 1], "none")

    def test_mips64_window_display_uses_serial_console_instead_of_ramfb(self) -> None:
        command = qemu_test.qemu_command(
            "mips64", systoolchain_qemu("mips64"), Path("kernel.bin"), "sdl", False, Path("disk.img")
        )
        self.assertIn("vc:80Cx24C", command)
        self.assertIn("stdio", command)
        self.assertNotIn("ramfb", command)
        self.assertEqual(command[command.index("-vga") + 1], "none")

    def test_ppc_window_display_uses_serial_console(self) -> None:
        command = qemu_test.qemu_command(
            "ppc", systoolchain_qemu("ppc"), Path("kernel.bin"), "sdl", False, Path("disk.img")
        )
        self.assertIn("vc:80Cx24C", command)
        self.assertIn("stdio", command)
        self.assertNotIn("ramfb", command)


if __name__ == "__main__":
    unittest.main()
