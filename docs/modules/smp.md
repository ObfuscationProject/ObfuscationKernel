# SMP

`ok::smp::CpuTopology` tracks boot, starting, online, halted, and offline CPUs.
The scheduler records per-CPU current process state and can schedule a process on
a specific CPU. This is still a cooperative model, but it gives architecture
bring-up and scheduler code a concrete SMP contract.

`TopologyMode` describes the CPU topology policy:

- `single_core`: only the boot CPU is expected online.
- `symmetric`: multiple equivalent CPUs are available.
- `numa_aware`: CPU records carry NUMA domain placement for future allocators.

`SpinLock`, `ScopedSpinLock`, and `PerCpu<T>` provide the minimal primitives
needed by shared kernel modules without dynamic allocation.

## CPU discovery

`ok/boot/cpu_topology.hpp` defines `CpuTopologyMap`, a constant-initializable
record of up to `smp::max_cpus` firmware-described CPUs (hardware id, enabled,
boot), the start mechanism the description supports and, when there is none,
the reason the platform must run UP. Parsing never starts a CPU.

- `parse_fdt_cpus` (`ok/boot/fdt.hpp`) reads `/cpus` (one or two address
  cells, zero size cells), each `device_type = "cpu"` child's `reg`, `status`
  and `enable-method`, `timebase-frequency` and a root-level PSCI node. The
  caller names the binding with `FdtCpuFlavor`. Arm selects `psci_hvc` or
  `psci_smc` only for PSCI 0.2/1.0 with an `hvc`/`smc` method and when every
  enabled secondary names `psci`; `cpu_on` defaults to the SMC64 id. RISC-V
  selects `riscv_msip` (M-mode release through the CLINT) for two or more
  enabled harts. The FDT header's `boot_cpuid_phys` marks the boot CPU when it
  names an enabled CPU.
- `find_acpi_rsdp` and `parse_acpi_madt` (`ok/boot/acpi.hpp`) read physical
  memory only through a `PhysicalReader` (function pointer plus context), so
  hosted tests supply synthetic memory. The XSDT is preferred for revision 2+
  RSDPs; root and MADT checksums and every length are checked against the
  reader's window. Enabled local APIC and x2APIC entries are present CPUs,
  online-capable ones are recorded disabled, others are skipped. Two or more
  enabled CPUs select `x86_sipi`; the caller identifies the BSP with
  `mark_boot_cpu`.

UP reasons are `single-cpu`, `psci-absent`, `psci-0.1-unsupported`,
`psci-method-missing`, `psci-method-unsupported`, `enable-method-not-psci` and,
from `mark_boot_cpu`, `boot-cpu-not-described`. A start method is usable only
once `boot_known` is set. More CPUs than `max_cpus` set `truncated`; the FDT
parser keeps the boot CPU in the map. `scripts/test_cpu_topology.py` is the
model suite; with `--qemu-dir` it also checks DTBs dumped by QEMU virt.
