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


_DEFAULT_TLB = object()


def make_smp_output(arch: str, smp_line: str | None, tlb_line: "str | None | object" = _DEFAULT_TLB) -> str:
    """Boot output with an OK_SMP line; SMP profiles get their QEMU TLB line unless one (or None) is given."""
    if tlb_line is _DEFAULT_TLB:
        tlb_line = QEMU_TLB_LINES.get(arch)
    output = make_output(arch) + "OK_SMP scope=model topology=pass per_cpu=pass ap_start=unsupported\n"
    return output + "".join(line + "\n" for line in (smp_line, tlb_line) if line)


# The TLB line each SMP profile prints under its QEMU CPU model.
QEMU_TLB_LINES = {
    "x86_64": "OK_SMP_TLB scope=hardware asid=none pcid=off",
    "aarch64": "OK_SMP_TLB scope=hardware asid=16 pcid=off",
    "rv64": "OK_SMP_TLB scope=hardware asid=16 pcid=off",
}

def tsc_line(cpu: int, result: str = "sync", offset: int = 165, error: int = 552, residual: int | None = None,
             residual_error: int | None = None, tolerance: int | None = None, skew: str | None = None) -> str:
    """An x86_64 OK_TSC_SYNC line as the kernel prints it (cycles; 100 ns slack, 50 us bound at ~3 GHz)."""
    skew_field = f" skew_test={skew}" if skew is not None else ""
    residual = offset if residual is None else residual
    residual_error = error if residual_error is None else residual_error
    tolerance = 301 if tolerance is None else tolerance
    return (f"OK_TSC_SYNC scope=hardware cpu={cpu} result={result} invariant=0 tsc_adjust=0{skew_field} slack=301 "
            f"max_error=150001 offset={offset} error={error} adjusted=0 software={offset if result == 'offset' else 0} "
            f"residual={residual} residual_error={residual_error} tolerance={tolerance}\n")


def tsc_lines(secondaries: int) -> str:
    return "".join(tsc_line(cpu) for cpu in range(1, secondaries + 1))


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

    def validate_smp(self, arch: str, output: str, cpus: int, tsc_skew_test: bool = False) -> tuple[int, str, str]:
        stdout = io.StringIO()
        stderr = io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            code = qemu_test.validate_output(arch, output, 0, False, cpus, tsc_skew_test)
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
                    line = tsc_lines(3) + line + " tsc=sync"
                code, stdout, stderr = self.validate_smp(arch, make_smp_output(arch, line, QEMU_TLB_LINES[arch]), 4)
                self.assertEqual(code, 0, stderr)
                self.assertIn(f"OK_QEMU_SMP_TEST arch={arch} smp=4 cpus=4 online=4", stdout)
                fields = QEMU_TLB_LINES[arch].split()[2:]
                self.assertIn(f"tlb_{fields[0]} {fields[1]}", stdout)

    def test_smp_profiles_require_a_consistent_tlb_report(self) -> None:
        base = "OK_SMP scope=hardware cpus=2 online=2 ap_start=sipi ipi=pass"
        accepted = {
            "x86_64": ("asid=none pcid=off", "asid=12 pcid=on"),
            "aarch64": ("asid=8 pcid=off", "asid=16 pcid=off"),
            "rv64": ("asid=none pcid=off", "asid=9 pcid=off", "asid=16 pcid=off"),
        }
        rejected = {
            "x86_64": ("asid=none pcid=on", "asid=12 pcid=off", "asid=16 pcid=off", "pcid=off", "asid=none"),
            "aarch64": ("asid=none pcid=off", "asid=12 pcid=on", "asid=16 pcid=on", "asid=16"),
            "rv64": ("asid=17 pcid=off", "asid=16 pcid=on", "asid=0 pcid=off", "pcid=off"),
        }
        for arch in qemu_test.SMP_QEMU_ARCHES:
            line = tsc_lines(1) + base + " tsc=sync" if arch in qemu_test.TSC_SYNC_QEMU_ARCHES else base
            for mode in accepted[arch]:
                with self.subTest(arch=arch, mode=mode):
                    output = make_smp_output(arch, line, f"OK_SMP_TLB scope=hardware {mode}")
                    code, stdout, stderr = self.validate_smp(arch, output, 2)
                    self.assertEqual(code, 0, stderr)
                    self.assertIn(f"tlb_{mode}", stdout)
            for mode in rejected[arch]:
                with self.subTest(arch=arch, mode=mode):
                    output = make_smp_output(arch, line, f"OK_SMP_TLB scope=hardware {mode}")
                    code, stdout, stderr = self.validate_smp(arch, output, 2)
                    self.assertEqual(code, 14)
                    self.assertIn("TLB report invalid", stderr)
                    self.assertNotIn("OK_QEMU_SMP_TEST", stdout)
            with self.subTest(arch=arch, mode="missing"):
                for tlb in (None, "OK_SMP_TLB scope=model asid=16 pcid=off"):
                    code, stdout, stderr = self.validate_smp(arch, make_smp_output(arch, line, tlb), 2)
                    self.assertEqual(code, 14)
                    self.assertIn("did not report OK_SMP_TLB scope=hardware for -smp 2", stderr)
                    self.assertNotIn("OK_QEMU_SMP_TEST", stdout)

    def test_x86_64_smp_report_names_its_tsc_synchronization(self) -> None:
        self.assertEqual(qemu_test.TSC_SYNC_QEMU_ARCHES, ("x86_64",))
        base = "OK_SMP scope=hardware cpus=2 online=2 ap_start=sipi ipi=pass"
        code, stdout, stderr = self.validate_smp("x86_64", make_smp_output("x86_64", tsc_lines(1) + base + " tsc=sync"),
                                                 2)
        self.assertEqual(code, 0, stderr)
        self.assertIn("OK_QEMU_SMP_TEST arch=x86_64 smp=2 cpus=2 online=2 ap_start=sipi ipi=pass tsc=sync", stdout)
        # Missing or unknown, an unsynced CPU (never admitted, so never online),
        # or a correction where every QEMU vCPU reads one counter.
        for line in (base, f"{base} tsc=", f"{base} tsc=skewed", f"{base} tsc=unsynced", f"{base} tsc=offset"):
            with self.subTest(line=line):
                output = make_smp_output("x86_64", tsc_lines(1) + line)
                code, stdout, stderr = self.validate_smp("x86_64", output, 2)
                self.assertEqual(code, 14)
                self.assertIn("tsc", stderr)
                self.assertNotIn("OK_QEMU_SMP_TEST", stdout)

    def test_x86_64_tsc_lines_must_back_the_smp_field(self) -> None:
        smp = "OK_SMP scope=hardware cpus=3 online=3 ap_start=sipi ipi=pass tsc=sync"
        broken = {
            "missing cpu": tsc_line(1),
            "duplicate cpu": tsc_line(1) + tsc_line(1) + tsc_line(2),
            "unsynced cpu": tsc_line(1) + tsc_line(2, result="unsynced"),
            "corrected cpu": tsc_line(1) + tsc_line(2, result="offset", offset=-90_000, residual=10),
            "sync beyond bound": tsc_line(1) + tsc_line(2, offset=900, error=500),
            "too wide": tsc_line(1) + tsc_line(2, error=200_000),
            "no fields": tsc_line(1) + "OK_TSC_SYNC scope=hardware cpu=2 result=sync\n",
        }
        for name, lines in broken.items():
            with self.subTest(name=name):
                code, stdout, stderr = self.validate_smp("x86_64", make_smp_output("x86_64", lines + smp), 3)
                self.assertEqual(code, 14, stdout)
                self.assertIn("TSC synchronization", stderr)
        code, _, stderr = self.validate_smp("x86_64", make_smp_output("x86_64", tsc_lines(2) + smp), 3)
        self.assertEqual(code, 0, stderr)
        # The model-scope line of a hosted run is not a hardware report.
        model = "OK_TSC_SYNC scope=model estimator=pass decisions=pass sweep=pass\n"
        code, _, stderr = self.validate_smp("x86_64", make_smp_output("x86_64", model + tsc_lines(2) + smp), 3)
        self.assertEqual(code, 0, stderr)

    def test_x86_64_skew_knob_requires_a_detected_and_corrected_cpu(self) -> None:
        smp = "OK_SMP scope=hardware cpus=4 online=4 ap_start=sipi ipi=pass tsc={}"
        corrected = tsc_line(1, result="offset", skew="software", offset=-25_090_269_788, error=778, residual=-279,
                             residual_error=596, tolerance=1079)
        others = tsc_line(2, skew="none") + tsc_line(3, skew="none")
        code, stdout, stderr = self.validate_smp(
            "x86_64", make_smp_output("x86_64", corrected + others + smp.format("offset")), 4, True)
        self.assertEqual(code, 0, stderr)
        self.assertIn("tsc=offset", stdout)
        adjusted = corrected.replace("result=offset", "result=adjusted").replace("skew_test=software", "skew_test=tsc")
        code, _, stderr = self.validate_smp(
            "x86_64", make_smp_output("x86_64", adjusted + others + smp.format("adjusted")), 4, True)
        self.assertEqual(code, 0, stderr)
        broken = {
            "undetected": (tsc_line(1, skew="software") + others, "sync"),
            "knob absent": (tsc_line(1) + tsc_line(2) + tsc_line(3), "sync"),
            "knob did not run": (corrected.replace("skew_test=software", "skew_test=none") + others, "offset"),
            "residual too large": (corrected.replace("residual=-279", "residual=-5000") + others, "offset"),
            "summary disagrees": (corrected + others, "sync"),
            "other cpu corrected": (corrected + tsc_line(2, result="offset", skew="none", offset=-90_000, residual=3)
                                    + tsc_line(3, skew="none"), "offset"),
        }
        for name, (lines, summary) in broken.items():
            with self.subTest(name=name):
                code, _, stderr = self.validate_smp(
                    "x86_64", make_smp_output("x86_64", lines + smp.format(summary)), 4, True)
                self.assertEqual(code, 14, name)
                self.assertIn("TSC synchronization", stderr)
        # Without the knob the same corrected CPU fails: QEMU vCPUs share one counter.
        code, _, _ = self.validate_smp("x86_64", make_smp_output("x86_64", corrected + others + smp.format("offset")), 4)
        self.assertEqual(code, 14)

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
                for value in ("bogus", "unsynced"):
                    code, _, _ = self.validate_smp(arch, make_smp_output(arch, f"{line} tsc={value}"), 2)
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
                    code, stdout, stderr = self.validate_smp(arch, make_smp_output(arch, line, QEMU_TLB_LINES[arch]), 2)
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
                self.assertNotIn("tlb_asid", stdout)
            for line in claimed:
                with self.subTest(arch=arch, line=line):
                    code, _, _ = self.validate_smp(arch, make_smp_output(arch, line), 2)
                    self.assertEqual(code, 14)

    def test_summary_records_cpu_count_and_both_smp_scopes(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            kernel = Path(tmp) / "kernel.bin"
            kernel.write_bytes(b"ok")
            output = make_smp_output("aarch64", "OK_SMP scope=hardware cpus=2 online=2 ap_start=psci ipi=pass",
                                     QEMU_TLB_LINES["aarch64"])
            qemu_test.write_summary(kernel, "aarch64", output, 0, 0, 2)
            summary = (kernel.parent / "qemu-test-summary.txt").read_text(encoding="utf-8").splitlines()
        self.assertIn("smp.cpus=2", summary)
        self.assertIn("marker.OK_SMP_TLB=present", summary)
        self.assertIn("smp.tlb.asid=16", summary)
        self.assertIn("smp.tlb.pcid=off", summary)
        self.assertIn("marker.OK_SMP=present", summary)
        self.assertIn("scopes.OK_SMP=model,hardware", summary)
        self.assertIn("marker.OK_SMP_HARDWARE=present", summary)
        self.assertIn("smp.hardware.online=2", summary)
        self.assertIn("smp.hardware.ap_start=psci", summary)

    def test_summary_defaults_to_one_cpu_without_hardware_smp(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            kernel = Path(tmp) / "kernel.bin"
            kernel.write_bytes(b"ok")
            qemu_test.write_summary(kernel, "x86_64", make_smp_output("x86_64", None, None), 0, 0)
            summary = (kernel.parent / "qemu-test-summary.txt").read_text(encoding="utf-8").splitlines()
        self.assertIn("smp.cpus=1", summary)
        self.assertIn("scope.OK_SMP=model", summary)
        self.assertIn("scopes.OK_SMP=model", summary)
        self.assertIn("scopes.OK_IRQ=absent", summary)
        self.assertIn("marker.OK_SMP_HARDWARE=missing", summary)
        self.assertIn("marker.OK_SMP_TLB=missing", summary)
        self.assertFalse(any(line.startswith(("smp.hardware.", "smp.tlb.")) for line in summary))

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
