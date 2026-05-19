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
