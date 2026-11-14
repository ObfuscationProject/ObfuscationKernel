#!/usr/bin/env python3
"""Boot an ObfuscationKernel kernel.bin in QEMU and validate debug output."""

from __future__ import annotations

import argparse
import os
import select
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path


QEMU_DEBUG_EXIT_SUCCESS = 33
DEFAULT_QEMU_TIMEOUT_SECONDS = 100.0
VIRTUAL_DISK_SIZE = 16 * 1024 * 1024
BASE_COVERAGE_FIELDS = ("fs", "simplefs", "ext4", "user", "posix", "shell", "modes", "gui")
CAPABILITY_COVERAGE_FIELDS = {
    "display": ("framebuffer", "ramfb", "virtio_gpu"),
    "gpu": ("virtio_gpu",),
    "input": ("keyboard_input", "mouse_input"),
    "bus": ("pci_bus",),
    "usb": ("usb_hid",),
    "net": ("network_loopback",),
}
ALL_COVERAGE_FIELDS = BASE_COVERAGE_FIELDS + tuple(CAPABILITY_COVERAGE_FIELDS)
BOOTABLE_QEMU_ARCHES = ("i386", "x86_64", "aarch64", "arm32", "rv64", "rv32", "loongarch64", "mips", "mips64", "ppc")
BOOTABLE_QEMU_CAPABILITIES = (
    "serial_console", "framebuffer", "keyboard_input", "mouse_input", "pci_bus",
    "virtio_block", "virtio_gpu", "ramfb", "usb_hid", "network_loopback",
)
SERIAL_WINDOW_QEMU_ARCHES = ("mips", "mips64", "ppc")
SERIAL_WINDOW_QEMU_CAPABILITIES = tuple(
    capability for capability in BOOTABLE_QEMU_CAPABILITIES if capability != "ramfb"
)

CAPABILITIES_BY_ARCH = {
    arch: SERIAL_WINDOW_QEMU_CAPABILITIES if arch in SERIAL_WINDOW_QEMU_ARCHES else BOOTABLE_QEMU_CAPABILITIES
    for arch in BOOTABLE_QEMU_ARCHES
}
# Profiles whose kernel must start every QEMU vCPU; the rest report UP.
# The hardware OK_SMP line must precede OK_TEST_PASS: run_until_marker stops
# reading there on every profile without a debug exit device.
SMP_QEMU_ARCHES = ("x86_64", "aarch64", "rv64")
# Profiles whose OK_SMP line names the TSC synchronization result (tsc=),
# with one OK_TSC_SYNC line per started secondary before it.
TSC_SYNC_QEMU_ARCHES = ("x86_64",)
TSC_SYNC_RESULTS = ("sync", "adjusted", "offset", "unsynced")
# The kernel admits only a verified CPU, so an online one is never unsynced.
TSC_SYNC_VERIFIED = ("sync", "adjusted", "offset")
MAX_QEMU_CPUS = 8
# What OK_SMP_TLB may report on each SMP profile: ASID widths (none: every
# entry flushes the user TLB) and whether x86 PCIDs carry the tags.
SMP_TLB_MODES = {
    "x86_64": {("none", "off"), ("12", "on")},
    "aarch64": {("8", "off"), ("16", "off")},
    "rv64": {("none", "off"), *((str(bits), "off") for bits in range(1, 17))},
}


# CPU hotplug lines, checked when present on an SMP profile: the boot test
# point (CPU 1 stopped, sent nothing while parked, restarted and pinged) and
# the smp_hotplug_test knob's cycle count.
HOTPLUG_BOOT_CHECKS = ("stop", "parked_ipi", "restart", "ipi")


class QemuConfigurationError(RuntimeError):
    """Raised before QEMU starts when the requested launch profile is invalid."""


def normalize_arch(arch: str) -> str:
    aliases = {
        "x64": "x86_64",
        "arm": "arm32",
        "arm64": "aarch64",
        "riscv64": "rv64",
        "riscv32": "rv32",
        "loong64": "loongarch64",
        "loongarch": "loongarch64",
        "powerpc": "ppc",
    }
    return aliases.get(arch, arch)


def arch_capabilities(arch: str) -> set[str]:
    return set(CAPABILITIES_BY_ARCH.get(arch, ()))


def required_coverage_fields(arch: str) -> set[str]:
    capabilities = arch_capabilities(arch)
    fields = set(BASE_COVERAGE_FIELDS)
    for field, required_caps in CAPABILITY_COVERAGE_FIELDS.items():
        if any(capability in capabilities for capability in required_caps):
            fields.add(field)
    return fields


def coverage_statuses(arch: str, fields: dict[str, str]) -> dict[str, str]:
    required = required_coverage_fields(arch)
    statuses: dict[str, str] = {}
    for field in ALL_COVERAGE_FIELDS:
        if field in required:
            if field not in fields:
                statuses[field] = "missing"
            elif fields[field] == "1":
                statuses[field] = "pass"
            else:
                statuses[field] = "fail"
            continue
        statuses[field] = "pass" if fields.get(field) == "1" else "skip"
    return statuses


def create_virtual_disk(path: Path) -> None:
    with path.open("wb") as handle:
        handle.truncate(VIRTUAL_DISK_SIZE)


def virtio_disk_args(disk: Path) -> list[str]:
    return [
        "-drive",
        f"file={disk},format=raw,if=none,id=fsdisk",
        "-device",
        "virtio-blk-pci,drive=fsdisk",
    ]


def ramfb_args() -> list[str]:
    return ["-device", "ramfb"]


def virtio_mmio_input_args() -> list[str]:
    return ["-device", "virtio-keyboard-device", "-device", "virtio-mouse-device"]


def virtio_pci_input_args() -> list[str]:
    return ["-device", "virtio-keyboard-pci", "-device", "virtio-mouse-pci"]


def serial_args(arch: str, display: str) -> list[str]:
    if arch in SERIAL_WINDOW_QEMU_ARCHES and display != "none":
        return ["-serial", "vc:80Cx24C", "-serial", "stdio"]
    return ["-serial", "stdio"]


def qemu_command(
    arch: str, qemu: Path, kernel: Path, display: str, debug_exit: bool, disk: Path, cpus: int = 1
) -> list[str]:
    if arch not in BOOTABLE_QEMU_ARCHES:
        raise QemuConfigurationError(f"qemu-system boot is not implemented for {arch} yet")
    if not 1 <= cpus <= MAX_QEMU_CPUS:
        raise QemuConfigurationError(f"qemu -smp must be between 1 and {MAX_QEMU_CPUS}, got {cpus}")
    command = profile_command(arch, str(qemu), kernel, display, debug_exit, disk)
    if cpus > 1:
        command += ["-smp", str(cpus)]
    return command


def profile_command(
    arch: str, qemu_path: str, kernel: Path, display: str, debug_exit: bool, disk: Path
) -> list[str]:
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
        command += serial_args(arch, display)
        command += ramfb_args()
        command += virtio_mmio_input_args()
        command += virtio_disk_args(disk)
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
        command += serial_args(arch, display)
        command += ramfb_args()
        command += virtio_mmio_input_args()
        command += virtio_disk_args(disk)
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
        command += serial_args(arch, display)
        command += ramfb_args()
        command += virtio_pci_input_args()
        command += virtio_disk_args(disk)
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
        command += serial_args(arch, display)
        command += virtio_disk_args(disk)
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
        command += serial_args(arch, display)
        command += virtio_disk_args(disk)
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
    command += ramfb_args()
    command += virtio_disk_args(disk)
    if debug_exit:
        command += ["-device", "isa-debug-exit,iobase=0xf4,iosize=0x04"]
    return command


def run_kernel(
    arch: str,
    qemu: Path,
    kernel: Path,
    display: str,
    debug_exit: bool,
    timeout: float | None,
    cpus: int = 1,
) -> subprocess.CompletedProcess[str]:
    with tempfile.TemporaryDirectory(prefix="okernel-qemu-") as tmp:
        runnable_kernel = Path(tmp) / "kernel.bin"
        scratch_disk = Path(tmp) / "fs.img"
        shutil.copyfile(kernel, runnable_kernel)
        create_virtual_disk(scratch_disk)
        command = qemu_command(arch, qemu, runnable_kernel, display, debug_exit, scratch_disk, cpus)
        if not debug_exit:
            return run_until_marker(command, timeout)
        return subprocess.run(command, text=True, capture_output=True, check=False, timeout=timeout)


def run_until_marker(command: list[str], timeout: float | None) -> subprocess.CompletedProcess[str]:
    # Never mix select(fd) with buffered readline(): one read can prefetch the
    # final marker while the kernel descriptor is empty for the rest of boot.
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
    stdout = bytearray()
    stderr = bytearray()
    assert process.stdout is not None and process.stderr is not None
    streams = {process.stdout: stdout, process.stderr: stderr}
    deadline = None if timeout is None else time.monotonic() + timeout
    parsed = 0
    pass_seen = fail_seen = False
    timed_out = False
    while streams and not (pass_seen or fail_seen):
        if deadline is not None and time.monotonic() >= deadline:
            timed_out = True
            break
        wait = 0.1 if deadline is None else max(0.0, min(0.1, deadline - time.monotonic()))
        ready, _, _ = select.select(list(streams), [], [], wait)
        for stream in ready:
            chunk = os.read(stream.fileno(), 65536)
            if not chunk:
                del streams[stream]
                continue
            streams[stream].extend(chunk)
        while (end := stdout.find(b"\n", parsed)) >= 0:
            line = stdout[parsed:end]
            parsed = end + 1
            pass_seen |= line.startswith(b"OK_TEST_PASS ")
            fail_seen |= line.startswith(b"OK_TEST_FAIL ")

    if process.poll() is None:
        process.terminate()
    try:
        tail_stdout, tail_stderr = process.communicate(timeout=1.0)
    except subprocess.TimeoutExpired:
        process.kill()
        tail_stdout, tail_stderr = process.communicate()
    stdout.extend(tail_stdout or b"")
    stderr.extend(tail_stderr or b"")
    returncode = 0 if pass_seen or fail_seen else (124 if timed_out else process.returncode)
    if pass_seen or fail_seen:
        stderr.clear()
    if not (pass_seen or fail_seen) and returncode == 0:
        returncode = 124
    if returncode == 124:
        stderr.extend(b"qemu marker timeout\n")
    return subprocess.CompletedProcess(command, returncode, stdout.decode(errors="replace"),
                                       stderr.decode(errors="replace"))


def hardware_smp_fields(lines: list[str]) -> dict[str, str] | None:
    for line in reversed(lines):
        if line.startswith("OK_SMP "):
            fields = parse_fields(line)
            if fields.get("scope") == "hardware":
                return fields
    return None


def hardware_tlb_fields(lines: list[str]) -> dict[str, str] | None:
    for line in reversed(lines):
        if line.startswith("OK_SMP_TLB "):
            fields = parse_fields(line)
            if fields.get("scope") == "hardware":
                return fields
    return None


def tsc_sync_problem(lines: list[str], cpus: int, tsc: str, skew_test: bool) -> str | None:
    """Check the per-CPU OK_TSC_SYNC lines behind OK_SMP's tsc= field.

    Under QEMU TCG every vCPU reads one host counter, so each secondary must
    measure in sync. With the x86_tsc_skew_test knob CPU 1 starts skewed and
    must be detected and corrected (offset or adjusted) to a residual within
    the bound the kernel verified, plus its own measurement error.
    """
    reports: dict[int, dict[str, str]] = {}
    for line in lines:
        if line.startswith("OK_TSC_SYNC "):
            fields = parse_fields(line)
            if fields.get("scope") == "hardware" and fields.get("cpu", "").isdigit():
                if int(fields["cpu"]) in reports:
                    return f"cpu {fields['cpu']} reported twice"
                reports[int(fields["cpu"])] = fields
    if sorted(reports) != list(range(1, cpus)):
        return f"expected one OK_TSC_SYNC line for each of cpus 1..{cpus - 1}, got {sorted(reports)}"
    worst = "sync"
    for cpu, fields in sorted(reports.items()):
        result = fields.get("result")
        if result not in TSC_SYNC_VERIFIED:
            return f"cpu {cpu} result={result}"
        if TSC_SYNC_VERIFIED.index(result) > TSC_SYNC_VERIFIED.index(worst):
            worst = result
        try:
            slack, max_error = int(fields["slack"]), int(fields["max_error"])
            offset, error = int(fields["offset"]), int(fields["error"])
            residual, residual_error = int(fields["residual"]), int(fields["residual_error"])
            tolerance = int(fields["tolerance"])
        except (KeyError, ValueError):
            return f"cpu {cpu} lacks its measurement fields"
        skewed = skew_test and cpu == 1
        if skewed:
            if fields.get("skew_test") not in ("tsc", "software"):
                return f"cpu 1 skew_test={fields.get('skew_test')}, expected the knob to have skewed it"
            if result == "sync" or abs(offset) <= error + slack:
                return f"cpu 1 skew went undetected: result={result} offset={offset} error={error}"
        elif result != "sync":
            return f"cpu {cpu} result={result}; every QEMU vCPU reads one counter"
        # The kernel's own acceptance test, recomputed from what it printed.
        if max(error, residual_error) > max_error:
            return f"cpu {cpu} measured with error={error} residual_error={residual_error} above {max_error}"
        if result == "sync" and abs(offset) > error + slack:
            return f"cpu {cpu} sync with offset={offset} beyond error={error} + slack={slack}"
        if result != "sync" and abs(residual) > residual_error + tolerance:
            return f"cpu {cpu} residual={residual} beyond residual_error={residual_error} + tolerance={tolerance}"
    if worst != tsc:
        return f"OK_SMP tsc={tsc} but the worst OK_TSC_SYNC result is {worst}"
    return None


def hotplug_problem(arch: str, lines: list[str], cpus: int) -> tuple[str | None, str]:
    """Check the OK_SMP_HOTPLUG_BOOT and OK_SMP_HOTPLUG lines, when present.

    Returns (problem, summary): the summary names what was checked for the
    OK_QEMU_SMP_TEST line. Only SMP profiles may print them. The boot line
    comes at most once. The knob's line comes with every interrupt report (at
    runtime start, at process retirements, at shutdown), so its counters are
    cumulative: they never decrease, it always names the same CPU, and the
    last one is the verdict.
    """
    boot = [parse_fields(line) for line in lines if line.startswith("OK_SMP_HOTPLUG_BOOT ")]
    cycles = [parse_fields(line) for line in lines if line.startswith("OK_SMP_HOTPLUG ")]
    if not boot and not cycles:
        return None, ""
    if arch not in SMP_QEMU_ARCHES:
        return f"{arch} starts no secondary CPU but printed a hotplug line", ""
    if len(boot) > 1:
        return "OK_SMP_HOTPLUG_BOOT was printed more than once", ""
    summary = ""
    if boot:
        fields = boot[0]
        if fields.get("scope") != "hardware" or fields.get("cpu") != "1":
            return "OK_SMP_HOTPLUG_BOOT must name scope=hardware cpu=1", ""
        failed = [name for name in HOTPLUG_BOOT_CHECKS if fields.get(name) != "pass"]
        if failed:
            return "OK_SMP_HOTPLUG_BOOT failed " + ",".join(failed), ""
        summary += " hotplug_boot=pass"
    if cycles:
        reports = []
        for fields in cycles:
            try:
                reports.append((int(fields["cycles"]), int(fields["failures"]), int(fields["cpu"])))
            except (KeyError, ValueError):
                return "OK_SMP_HOTPLUG lacks cycles, failures or cpu", ""
            if fields.get("scope") != "hardware":
                return "OK_SMP_HOTPLUG must name scope=hardware", ""
        for previous, current in zip(reports, reports[1:]):
            if current[0] < previous[0] or current[1] < previous[1] or current[2] != previous[2]:
                return "OK_SMP_HOTPLUG counters went back or its CPU changed between reports", ""
        count, failures, cpu = reports[-1]
        if count <= 0 or failures != 0 or not 1 <= cpu < cpus:
            return f"OK_SMP_HOTPLUG cycles={count} failures={failures} cpu={cpu}", ""
        summary += f" hotplug_cycles={count}"
    return None, summary


def validate_smp(arch: str, lines: list[str], cpus: int, tsc_skew_test: bool = False) -> int:
    fields = hardware_smp_fields(lines)
    if fields is None:
        print(f"debug kernel did not report OK_SMP scope=hardware for -smp {cpus}", file=sys.stderr)
        return 14
    reported = " ".join(f"{key}={fields.get(key, 'missing')}" for key in ("cpus", "online", "ap_start", "ipi"))
    if arch in SMP_QEMU_ARCHES:
        expected = {"cpus": str(cpus), "online": str(cpus), "ipi": "pass"}
    else:
        expected = {"online": "1", "ap_start": "unsupported"}
    if any(fields.get(key) != value for key, value in expected.items()):
        wanted = " ".join(f"{key}={value}" for key, value in expected.items())
        print(f"debug kernel SMP report mismatch for -smp {cpus}: expected {wanted}, got {reported}", file=sys.stderr)
        return 14
    # How the started CPUs' TSCs agree with the boot CPU's; x86_64 reports it
    # once a secondary started, other profiles read one shared counter.
    tsc = fields.get("tsc")
    if (tsc is None and arch in TSC_SYNC_QEMU_ARCHES and int(fields.get("online", "1")) > 1) or (
            tsc is not None and tsc not in TSC_SYNC_VERIFIED):
        print(f"debug kernel SMP report for -smp {cpus} has tsc={tsc or 'missing'}, expected one of "
              f"{','.join(TSC_SYNC_VERIFIED)}", file=sys.stderr)
        return 14
    if arch in TSC_SYNC_QEMU_ARCHES and tsc is not None:
        problem = tsc_sync_problem(lines, cpus, tsc, tsc_skew_test)
        if problem:
            print(f"debug kernel TSC synchronization for -smp {cpus}: {problem}", file=sys.stderr)
            return 14
    reason = f" reason={fields['reason']}" if "reason" in fields else ""
    sync = f" tsc={tsc}" if tsc is not None else ""
    tlb = ""
    if arch in SMP_QEMU_ARCHES:
        tlb_fields = hardware_tlb_fields(lines)
        if tlb_fields is None:
            print(f"debug kernel did not report OK_SMP_TLB scope=hardware for -smp {cpus}", file=sys.stderr)
            return 14
        mode = (tlb_fields.get("asid", "missing"), tlb_fields.get("pcid", "missing"))
        if mode not in SMP_TLB_MODES[arch]:
            print(f"debug kernel TLB report invalid for {arch}: asid={mode[0]} pcid={mode[1]}", file=sys.stderr)
            return 14
        tlb = f" tlb_asid={mode[0]} pcid={mode[1]}"
    problem, hotplug = hotplug_problem(arch, lines, cpus)
    if problem:
        print(f"debug kernel CPU hotplug for -smp {cpus}: {problem}", file=sys.stderr)
        return 14
    print(f"OK_QEMU_SMP_TEST arch={arch} smp={cpus} {reported}{reason}{sync}{tlb}{hotplug}")
    return 0


def validate_output(arch: str, output: str, returncode: int, accept_debug_exit: bool, cpus: int = 1,
                    tsc_skew_test: bool = False) -> int:
    lines = output.splitlines()
    fail_lines = [line for line in lines if line.startswith("OK_TEST_FAIL ")]
    if fail_lines:
        print(f"debug kernel reported failure: {fail_lines[-1]}", file=sys.stderr)
        return 13

    if accept_debug_exit and returncode != QEMU_DEBUG_EXIT_SUCCESS:
        if returncode == 124:
            print("qemu timed out before OK_TEST_PASS", file=sys.stderr)
            return 12
        print(f"qemu did not exit through debug-exit success path: returncode={returncode}", file=sys.stderr)
        return returncode if returncode != 0 else 11
    if not accept_debug_exit and returncode != 0:
        if returncode == 124:
            print("qemu timed out before OK_TEST_PASS", file=sys.stderr)
            return 12
        print(f"qemu exited with returncode={returncode}", file=sys.stderr)
        return returncode

    if "OK_MODE debug" not in lines:
        print("kernel did not boot in debug mode", file=sys.stderr)
        return 3
    if not any(line.startswith("OK_DEBUG boot=complete") for line in lines):
        print("debug kernel did not report boot completion", file=sys.stderr)
        return 4

    pass_lines = [line for line in lines if line.startswith("OK_TEST_PASS ")]
    if not pass_lines:
        print("debug kernel did not report OK_TEST_PASS", file=sys.stderr)
        return 5

    fields = parse_fields(pass_lines[-1])
    if fields.get("arch") != arch:
        print(f"debug kernel arch mismatch: expected {arch}, got {fields.get('arch')}", file=sys.stderr)
        return 6
    if int(fields.get("debug_test_points", "0")) == 0:
        print("debug kernel did not run debug test points", file=sys.stderr)
        return 7

    coverage = coverage_statuses(arch, fields)
    required = required_coverage_fields(arch)
    for field in ALL_COVERAGE_FIELDS:
        if field not in required:
            if coverage[field] == "skip":
                print(f"OK_QEMU_SKIP arch={arch} coverage={field} reason=missing_capability")
            continue
        if coverage[field] == "missing":
            print(f"debug kernel did not report required coverage field: {field}", file=sys.stderr)
            return 8
        if coverage[field] == "fail":
            print(f"debug kernel did not pass required coverage field: {field}", file=sys.stderr)
            return 8

    if fields.get("display") == "1" and int(fields.get("display_checksum", "0")) == 0:
        print("display driver did not produce a framebuffer checksum", file=sys.stderr)
        return 9
    if fields.get("display") == "1" and not any(line.startswith("OK_DISPLAY_TEXT ") for line in lines):
        print("display driver did not report boot text", file=sys.stderr)
        return 10
    print(f"OK_QEMU_NET_TEST arch={arch} udp=pass tcp=pass")
    if cpus > 1:
        return validate_smp(arch, lines, cpus, tsc_skew_test)
    return 0


def write_summary(
    kernel: Path, arch: str, output: str, qemu_returncode: int, validation_code: int, cpus: int = 1
) -> None:
    summary = kernel.parent / "qemu-test-summary.txt"
    lines = output.splitlines()
    pass_lines = [line for line in lines if line.startswith("OK_TEST_PASS ")]
    fields = parse_fields(pass_lines[-1]) if pass_lines else {}
    roadmap_markers = [
        "OK_MODULES",
        "OK_VM",
        "OK_PROC",
        "OK_ELF",
        "OK_USERLAND",
        "OK_VFS",
        "OK_DEVFS",
        "OK_PIPE",
        "OK_TTY",
        "OK_LINUX_ABI",
        "OK_LINUX_SYSCALLS",
        "OK_DRIVER_ABI",
        "OK_GUI",
        "OK_MODULE_METADATA",
        "OK_NETDEV",
        "OK_SOCK",
        "OK_BLOCK",
        "OK_EXT4_READONLY",
        "OK_SMP",
        "OK_SMP_TLB",
        "OK_IRQ",
        "OK_PREEMPT",
        "OK_TEST_FAIL",
    ]
    marker_status = {
        marker: any(line.startswith(marker + " ") for line in lines)
        for marker in roadmap_markers
    }
    marker_scopes = {
        marker: next((parse_fields(line).get("scope", "unspecified")
                      for line in reversed(lines) if line.startswith(marker + " ")), "absent")
        for marker in roadmap_markers
    }
    marker_scope_lists = {
        marker: ",".join(dict.fromkeys(parse_fields(line).get("scope", "unspecified")
                                       for line in lines if line.startswith(marker + " "))) or "absent"
        for marker in roadmap_markers
    }
    smp_hardware = hardware_smp_fields(lines)
    smp_tlb = hardware_tlb_fields(lines)
    coverage = coverage_statuses(arch, fields)
    with summary.open("w", encoding="utf-8") as handle:
        handle.write(f"arch={arch}\n")
        handle.write(f"kernel={kernel}\n")
        handle.write(f"qemu_returncode={qemu_returncode}\n")
        handle.write(f"validation_code={validation_code}\n")
        handle.write(f"capabilities={','.join(CAPABILITIES_BY_ARCH.get(arch, ()))}\n")
        handle.write(f"smp.cpus={cpus}\n")
        handle.write(f"ok_mode={'OK_MODE debug' in lines}\n")
        handle.write(f"boot_complete={any(line.startswith('OK_DEBUG boot=complete') for line in lines)}\n")
        handle.write(f"ok_test_pass={bool(pass_lines)}\n")
        if fields:
            for key in sorted(fields):
                handle.write(f"field.{key}={fields[key]}\n")
        for field in ALL_COVERAGE_FIELDS:
            handle.write(f"coverage.{field}={coverage[field]}\n")
            if coverage[field] == "skip":
                handle.write(f"skip.{field}=missing_capability\n")
        for marker, present in marker_status.items():
            handle.write(f"marker.{marker}={'present' if present else 'missing'}\n")
            handle.write(f"scope.{marker}={marker_scopes[marker]}\n")
            handle.write(f"scopes.{marker}={marker_scope_lists[marker]}\n")
        handle.write(f"marker.OK_SMP_HARDWARE={'present' if smp_hardware is not None else 'missing'}\n")
        for key in sorted(smp_hardware or {}):
            handle.write(f"smp.hardware.{key}={smp_hardware[key]}\n")
        for key in sorted(smp_tlb or {}):
            handle.write(f"smp.tlb.{key}={smp_tlb[key]}\n")
        for marker, prefix in (("OK_SMP_HOTPLUG_BOOT", "smp.hotplug_boot"), ("OK_SMP_HOTPLUG", "smp.hotplug")):
            fields = next((parse_fields(line) for line in reversed(lines) if line.startswith(marker + " ")), {})
            for key in sorted(fields):
                handle.write(f"{prefix}.{key}={fields[key]}\n")


def parse_fields(line: str) -> dict[str, str]:
    fields: dict[str, str] = {}
    for token in line.strip().split()[1:]:
        if "=" in token:
            key, value = token.split("=", 1)
            fields[key] = value
    return fields


def smp_cpu_count(value: str) -> int:
    try:
        cpus = int(value)
    except ValueError:
        raise argparse.ArgumentTypeError(f"invalid CPU count: {value}") from None
    if not 1 <= cpus <= MAX_QEMU_CPUS:
        raise argparse.ArgumentTypeError(f"CPU count must be between 1 and {MAX_QEMU_CPUS}")
    return cpus


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--arch", required=True)
    parser.add_argument("--kernel", required=True, type=Path, help="Path to the compiled kernel.bin")
    parser.add_argument("--qemu", required=True, type=Path, help="SysToolchain QEMU executable")
    parser.add_argument("--display", default="none")
    parser.add_argument("--no-debug-exit", action="store_true")
    parser.add_argument("--timeout", type=float, default=DEFAULT_QEMU_TIMEOUT_SECONDS)
    parser.add_argument("--smp", type=smp_cpu_count, default=1,
                        help=f"QEMU vCPU count (1..{MAX_QEMU_CPUS}); above 1 requires the hardware OK_SMP marker")
    parser.add_argument("--tsc-skew-test", action="store_true",
                        help="the x86_64 kernel was built with x86_tsc_skew_test: CPU 1 must be found skewed "
                             "and corrected")
    args = parser.parse_args()

    arch = normalize_arch(args.arch)
    kernel = args.kernel.resolve()
    qemu = args.qemu.resolve()
    if not kernel.exists():
        print(f"kernel image does not exist: {kernel}", file=sys.stderr)
        return 2
    if not qemu.is_file():
        print(f"SysToolchain QEMU executable does not exist: {qemu}", file=sys.stderr)
        return 2

    use_debug_exit = not args.no_debug_exit and arch in ("i386", "x86_64")
    if args.display != "none" and arch in SERIAL_WINDOW_QEMU_ARCHES:
        print(
            f"QEMU_DISPLAY_FALLBACK arch={arch} display=serial-vc reason=machine_has_no_fw_cfg_ramfb",
            file=sys.stderr,
        )
    try:
        result = run_kernel(arch, qemu, kernel, args.display, use_debug_exit, args.timeout, args.smp)
    except QemuConfigurationError as error:
        print(str(error), file=sys.stderr)
        write_summary(kernel, arch, "", 127, 11, args.smp)
        return 11
    except subprocess.TimeoutExpired as error:
        stdout = error.stdout or ""
        stderr = error.stderr or ""
        if isinstance(stdout, bytes):
            stdout = stdout.decode(errors="replace")
        if isinstance(stderr, bytes):
            stderr = stderr.decode(errors="replace")
        if stdout:
            print(stdout, end="")
        if stderr:
            print(stderr, end="", file=sys.stderr)
        print("qemu timed out before OK_TEST_PASS", file=sys.stderr)
        write_summary(kernel, arch, stdout, 124, 12, args.smp)
        return 12
    if result.stdout:
        print(result.stdout, end="")
    if result.stderr:
        print(result.stderr, end="", file=sys.stderr)
    validation_code = validate_output(arch, result.stdout, result.returncode, use_debug_exit, args.smp,
                                      args.tsc_skew_test)
    write_summary(kernel, arch, result.stdout, result.returncode, validation_code, args.smp)
    return validation_code


if __name__ == "__main__":
    raise SystemExit(main())
