# ObfuscationKernel Roadmap and Test Requirements

This document contains only the implementation roadmap and test requirements for growing ObfuscationKernel into a UNIX-like C++23/xmake kernel with an inheritance-based kernel-module architecture, Linux-compatible syscall layer, and a future Linux-driver compatibility path.

## Agent Mission

Implement the roadmap incrementally. Every change must preserve the existing boot path, xmake workflow, freestanding C++23 constraints, and QEMU regression tests.

The Agent must work in small, verifiable milestones. Each milestone must include:

1. A clear implementation target.
2. Minimal architectural changes needed for that target.
3. New or updated tests.
4. Passing build and boot validation before moving to the next milestone.
5. Documentation updates only when they describe a new verified capability.

## Global Implementation Rules

- Use C++23.
- Keep xmake as the only project build entry point.
- Keep the freestanding kernel free of exceptions, hosted allocation-heavy containers, and unnecessary standard-library runtime assumptions.
- Prefer fixed-capacity kernel containers unless a real kernel heap allocator is explicitly implemented and tested.
- Keep runtime polymorphism at hardware, bus, driver, filesystem, scheduler, syscall, and module boundaries.
- Use inheritance for kernel modules and driver families.
- Use concepts/templates only for local zero-overhead extension points.
- Do not break the existing `okernel` static-library target or `okernel_image` boot target.
- Do not add a feature unless there is a QEMU, debug-suite, unit-style test point, or deterministic shell-based validation path.
- Do not claim Linux driver compatibility until a real compatibility surface and at least one working compatibility driver are demonstrated.

## Roadmap Overview

| Phase | Goal | Main Deliverable |
| --- | --- | --- |
| P0 | Stabilise baseline | Reliable matrix build and QEMU validation |
| P1 | Formal kernel modules | `KernelModule`, `ModuleManager`, service registry, dependency lifecycle |
| P2 | Real memory isolation | Page tables, kernel/user spaces, user-pointer validation |
| P3 | UNIX process model | ELF loader, process/thread lifecycle, `fork`/`execve`/`wait` path |
| P4 | VFS and device model | Mount tree, vnode/inode abstraction, devfs, pipes, TTY |
| P5 | Linux syscall compatibility | Architecture-aware syscall ABI, errno mapping, libc smoke tests |
| P6 | Driver compatibility layer | OKernel driver ABI, Linux source-compatibility shims, selected driver ports |
| P7 | Network and storage expansion | Virtio-net, socket syscall layer, improved block cache and filesystems |
| P8 | SMP and preemption | Real AP bring-up, timer preemption, per-CPU scheduler queues |

---

# P0: Baseline Stabilisation

## Implementation Requirements

- Ensure all supported architecture profiles have a consistent xmake configuration path.
- Ensure bootable targets have deterministic QEMU commands.
- Split platform capabilities into explicit flags:
  - serial console
  - framebuffer
  - keyboard input
  - mouse input
  - PCI bus
  - virtio block
  - virtio GPU
  - ramfb
  - USB HID
  - network loopback
- Make debug tests capability-aware instead of assuming every bootable architecture has the same devices.
- Ensure QEMU test scripts emit clear failure reasons for:
  - build failure
  - QEMU executable missing
  - timeout
  - missing `OK_MODE`
  - missing `OK_DEBUG boot=complete`
  - missing `OK_TEST_PASS`
  - missing required coverage field
  - wrong architecture marker
- Add a generated test summary file under the build directory.

## Test Requirements

### Required commands

```sh
xmake f -c -m debug -a x86_64
xmake -y -b okernel
xmake -y -b okernel_image
xmake qemu-test
```

```sh
xmake profile-matrix
xmake qemu-matrix
```

### Pass criteria

- `xmake -y -b okernel` succeeds.
- `xmake -y -b okernel_image` succeeds for bootable targets.
- QEMU output contains:

```text
OK_MODE debug
OK_DEBUG boot=complete
OK_TEST_PASS arch=<arch>
```

- Matrix tasks fail only with explicit, actionable reasons.
- Capability-specific tests are skipped only when the platform capability is absent and the skip is reported.

---

# P1: Formal Kernel Module System

## Implementation Requirements

Create a first-class kernel module system.

### Required abstractions

- `KernelModule`
- `ModuleId`
- `ModuleManifest`
- `ModuleDependency`
- `ModuleState`
- `ModuleManager`
- `KernelService`
- `ServiceRegistry`

### Required module lifecycle

Each module must support:

```cpp
probe()
init()
start()
stop()
shutdown()
```

### Required states

- `created`
- `probed`
- `initialized`
- `started`
- `stopped`
- `failed`

### Required module metadata

Each module manifest must include:

- stable name
- version
- module class
- dependency list
- exported service list
- required service list
- whether it is built-in or loadable
- init priority

### Required built-in modules

Convert or wrap the following into module form:

- architecture module
- memory module
- interrupt module
- scheduler module
- SMP module
- IPC module
- syscall module
- driver core module
- VFS module
- POSIX module
- user-mode module
- debug shell module

### Required service registry

The service registry must support:

- registering service interfaces
- querying service interfaces by stable service ID
- detecting duplicate services
- rejecting missing required services
- avoiding unsafe `dynamic_cast` in core boot paths
- preserving RTTI availability for diagnostics

### Required dependency handling

`ModuleManager` must:

- sort modules by dependency order
- reject dependency cycles
- reject missing dependencies
- stop modules in reverse order
- record failure state and failure message

## Test Requirements

### New debug markers

Add module-specific boot markers:

```text
OK_MODULE name=<name> state=started
OK_MODULES count=<n> failed=0
```

### Required tests

- Register a module with no dependencies.
- Register multiple modules with dependencies.
- Reject duplicate module names.
- Reject missing dependency.
- Reject dependency cycle.
- Start modules in dependency order.
- Stop modules in reverse dependency order.
- Query an exported service.
- Reject querying a missing service.
- Verify old boot path still reaches `OK_TEST_PASS`.

### Required commands

```sh
xmake f -c -m debug -a x86_64
xmake qemu-test
xmake profile-matrix
```

### Pass criteria

- All built-in core subsystems are visible as modules.
- `OK_MODULES failed=0` is emitted.
- Existing debug fields still pass.
- No hard-coded direct dependency is introduced where a registered service should be used.

---

# P2: Real Memory Isolation

## Implementation Requirements

Implement real virtual memory rather than only linear mapping metadata.

### Required abstractions

- `PageTable`
- `PageTableEntry`
- `VirtualMemoryManager`
- `KernelAddressSpace`
- `UserAddressSpace`
- `VmArea`
- `UserPtr<T>`
- `UserSlice<T>`
- `CopyResult`

### Required features

- physical frame allocation with reserved kernel-image regions
- kernel higher-half or architecture-appropriate kernel mapping mode
- per-process user address spaces
- page permissions:
  - read
  - write
  - execute
  - user
  - global
  - device
  - no-cache
- map and unmap page ranges
- address-space clone primitive
- copy-on-write placeholder API, even if initially not fully optimised
- page fault classification
- user pointer validation
- `copy_from_user`
- `copy_to_user`
- safe string copy from user memory
- safe vector copy from user memory

### Syscall safety changes

Replace raw syscall pointer casts with safe user-copy helpers for:

- path strings
- read/write buffers
- `stat` output
- `getcwd` output
- `iovec`
- `timespec`
- `uname`
- `sysinfo`
- `rlimit`
- `arch_prctl` output

## Test Requirements

### Required debug tests

- Map and unmap one kernel page.
- Map and unmap one user page.
- Reject null user pointer where required.
- Reject unmapped user pointer.
- Reject write into read-only user mapping.
- Copy valid user buffer into kernel buffer.
- Copy kernel buffer into valid user buffer.
- Copy bounded C string from user memory.
- Dispatch at least one syscall using only `UserPtr` helpers.

### Required markers

```text
OK_VM kernel_map=pass user_map=pass user_copy=pass fault=pass
```

### Required commands

```sh
xmake f -c -m debug -a x86_64
xmake qemu-test
```

For every architecture with page-table support:

```sh
xmake qemu-test -a <arch>
```

### Pass criteria

- No syscall handler directly dereferences a user-provided pointer unless it is explicitly marked as a temporary test-only path.
- Invalid user pointers return stable negative error results.
- Existing POSIX debug tests still pass.

---

# P3: UNIX Process and Thread Model

## Implementation Requirements

Implement a real process model suitable for UNIX compatibility.

### Required process objects

- `Process`
- `Thread`
- `Task`
- `ProcessGroup`
- `Session`
- `Credentials`
- `SignalState`
- `FileDescriptorTable`
- `MemoryMap`
- `WaitQueue`

### Required lifecycle

- create kernel thread
- create user process
- create user thread
- runnable/running/blocked/zombie/exited states
- exit code storage
- parent-child relationship
- wait state
- reparenting to init process
- scheduler integration

### Required ELF loader

Implement an ELF64 loader first, then ELF32 if needed.

The loader must:

- validate ELF magic
- validate architecture
- validate program headers
- map `PT_LOAD` segments
- apply permissions
- zero `.bss`
- create initial user stack
- pass argc/argv/envp/auxv
- set entry point and stack pointer

### Required syscalls

Implement or significantly improve:

- `clone`
- `fork`
- `vfork` if desired
- `execve`
- `wait4`
- `exit`
- `exit_group`
- `getpid`
- `getppid`
- `gettid`
- `set_tid_address`
- `sched_yield`
- `nanosleep`

### Required scheduling features

- per-thread scheduling rather than process-only scheduling
- blocking and wakeup
- per-CPU run queues as a supported mode
- idle thread per CPU
- scheduler statistics
- voluntary yield
- timer tick accounting

## Test Requirements

### Required kernel tests

- Create two kernel threads and schedule both.
- Create one user process from an embedded ELF test binary.
- Verify user process reaches a syscall and exits.
- Verify parent can `wait4` child.
- Verify `fork` duplicates FD table and memory map metadata.
- Verify `execve` replaces address space and preserves selected process metadata.
- Verify `exit_group` exits all threads in a process.
- Verify zombie cleanup after wait.

### Required userland smoke tests

Add tiny freestanding user programs:

- `hello`: writes to stdout and exits.
- `args`: validates argc/argv.
- `forkwait`: forks child and waits.
- `exec`: execs another test binary.
- `fd`: opens, writes, reads, closes a file.

### Required markers

```text
OK_PROC create=pass schedule=pass exit=pass wait=pass
OK_ELF load=pass entry=pass stack=pass
OK_USERLAND hello=pass fd=pass fork=pass exec=pass
```

### Required commands

```sh
xmake f -c -m debug -a x86_64
xmake qemu-test
```

Optional later:

```sh
xmake userland-test
```

### Pass criteria

- A user process can execute at least one instruction path and issue a syscall.
- `execve` can load a test ELF from the kernel filesystem or embedded initramfs.
- `fork`/`wait4` have deterministic debug tests.
- Existing single-process fallback tests are either removed or renamed as compatibility fallbacks.

---

# P4: VFS, Mounts, Devices, Pipes, and TTY

## Implementation Requirements

Replace the RAM-only VFS model with a mountable UNIX-style VFS.

### Required abstractions

- `Vnode`
- `Inode`
- `SuperBlock`
- `File`
- `FileOperations`
- `DirectoryOperations`
- `Mount`
- `MountNamespace`
- `DeviceNode`
- `Pipe`
- `TtyDevice`

### Required filesystem features

- mount tree
- lookup with `.` and `..`
- absolute and relative path resolution
- symlink handling
- hard-link count model
- directory removal when empty
- file truncation
- file offset per open file
- permission metadata
- special files
- `/dev`
- `/proc` minimal model
- `/tmp`
- initramfs or embedded root filesystem

### Required device nodes

Implement at least:

- `/dev/null`
- `/dev/zero`
- `/dev/random` or `/dev/urandom`
- `/dev/console`
- `/dev/tty0`
- block device node for root disk

### Required pipe and TTY syscalls

- `pipe`
- `pipe2`
- `read`
- `write`
- `poll`
- `select` or `pselect6`
- `ioctl` minimal TTY operations

## Test Requirements

### Required filesystem tests

- mount RAMFS at `/`
- mount devfs at `/dev`
- create/read/write/truncate regular file
- create/list/remove directory
- reject removing non-empty directory
- create/read symlink
- resolve `.` and `..`
- open device node and perform expected read/write
- create pipe and transfer bytes
- verify pipe blocking semantics in non-blocking mode

### Required shell tests

The debug shell must validate:

```sh
ls /
ls /dev
cat /dev/zero
touch /tmp/a
echo hello > /tmp/a
cat /tmp/a
mkdir /tmp/d
rm /tmp/a
```

Redirection can be implemented either in the shell or as an explicit test helper.

### Required markers

```text
OK_VFS mount=pass path=pass file=pass dir=pass symlink=pass
OK_DEVFS null=pass zero=pass console=pass
OK_PIPE create=pass transfer=pass poll=pass
OK_TTY console=pass ioctl=pass
```

### Pass criteria

- File descriptor operations are backed by `FileOperations`, not direct RAM node assumptions.
- Device nodes behave through driver-backed file operations.
- POSIX file tests continue to pass.

---

# P5: Linux Syscall Compatibility Layer

## Implementation Requirements

Implement Linux-compatible syscall dispatch as a compatibility layer above OKernel primitives.

### Required architecture-aware syscall ABI

For each supported userland architecture, define:

- syscall instruction or trap mechanism
- syscall number register
- argument registers
- return register
- error return convention
- clobbered registers
- restart behavior placeholder

Start with:

- x86_64 Linux syscall ABI
- i386 int 0x80 or sysenter path later
- aarch64 svc ABI later
- riscv ecall ABI later

### Required syscall layer design

- `LinuxSyscallAbi`
- `LinuxSyscallFrame`
- `LinuxSyscallDispatcher`
- `ErrnoMapper`
- `LinuxCompatProcess`
- `LinuxAuxvBuilder`
- `LinuxVdsoPlaceholder`

### Required errno behavior

Map internal `StatusCode` to Linux-style negative errno values.

Minimum mappings:

- invalid argument -> `-EINVAL`
- not found -> `-ENOENT`
- no memory -> `-ENOMEM`
- unsupported -> `-ENOSYS`
- would block -> `-EAGAIN`
- already exists -> `-EEXIST`
- overflow -> `-EOVERFLOW`
- not initialized -> `-EIO` or subsystem-specific error

### Required syscall groups

Implement enough for static libc or minimal musl startup:

- process identity
- file I/O
- path operations
- memory management
- time
- signals as safe no-op placeholders where acceptable
- futex minimal behavior
- random
- uname
- sysinfo
- rlimit
- arch-specific TLS setup

### Required libc target

Choose a minimal smoke-test target:

- static musl-style ELF if practical
- otherwise custom tiny Linux-ABI user programs first

## Test Requirements

### Required syscall ABI tests

- Verify syscall arguments are decoded from the correct registers.
- Verify return values use Linux negative errno.
- Verify unknown syscall returns `-ENOSYS`.
- Verify invalid user pointer returns `-EFAULT`.
- Verify `write(1, ...)` reaches console.
- Verify `openat/read/write/close` works from user program.
- Verify `mmap/brk/munmap` works from user program.
- Verify `arch_prctl(ARCH_SET_FS)` sets TLS base on x86_64.

### Required userland tests

Add user programs:

- `linux_write`
- `linux_openat`
- `linux_mmap`
- `linux_getdents`
- `linux_time`
- `linux_futex_basic`
- `linux_tls`

### Required markers

```text
OK_LINUX_ABI arch=x86_64 args=pass errno=pass unknown=pass
OK_LINUX_SYSCALLS file=pass memory=pass time=pass futex=pass tls=pass
```

### Pass criteria

- Linux syscall compatibility is separated from native OKernel syscall internals.
- Native syscall tests and Linux compatibility tests both pass.
- No syscall handler directly trusts user memory.

---

# P6: Kernel Driver ABI and Linux Driver Compatibility Path

## Implementation Requirements

Do not start by loading arbitrary Linux `.ko` files. First implement a stable OKernel driver ABI, then add a Linux source-compatibility shim for selected driver classes.

### Required OKernel driver ABI

- `OkDriverModule`
- `OkDriverManifest`
- `OkDriverOps`
- `OkBusType`
- `OkDevice`
- `OkDeviceId`
- `OkProbeContext`
- `OkDmaBuffer`
- `OkIrqHandle`
- `OkMmioRegion`
- `OkIoPortRegion`
- `OkWorkQueue`
- `OkTimer`
- `OkRefCount`
- `OkSpinLock`
- `OkMutex`

### Required driver lifecycle

- match
- probe
- attach
- start
- suspend
- resume
- remove
- shutdown

### Required bus model

- PCI
- platform/MMIO
- virtio
- USB later

### Required Linux source-compatibility shim

Implement only a controlled subset first:

- Linux-like `module_init`
- Linux-like `module_exit`
- `pci_driver`
- `pci_device_id`
- `probe`
- `remove`
- `request_irq`
- `free_irq`
- `ioremap`
- `iounmap`
- `readl/writel`
- `spinlock_t`
- `mutex`
- `kmalloc/kfree`
- `printk`
- selected `errno`
- selected `list_head`
- selected workqueue/timer stubs

### First target drivers

Use simple controlled targets before real Linux drivers:

1. OKernel native fake PCI driver.
2. Linux-shim fake PCI driver.
3. Virtio block source-compatible experiment.
4. Virtio net source-compatible experiment.

### Loadable module format

Implement after built-in ABI is stable:

- ELF relocatable parser
- symbol table
- relocation handling
- exported kernel symbol table
- module dependency metadata
- module signature placeholder
- unload safety rules

## Test Requirements

### Required native driver tests

- Register fake PCI device.
- Match native OKernel PCI driver.
- Probe and attach driver.
- Map fake BAR.
- Register fake IRQ.
- Remove driver cleanly.

### Required Linux shim tests

- Compile a fake Linux-style PCI driver against shim headers.
- Register `pci_driver`.
- Match fake PCI device.
- Call `probe`.
- Use `ioremap/readl/writel`.
- Use `kmalloc/kfree`.
- Use `spin_lock/spin_unlock`.
- Call `remove`.

### Required loadable module tests

When loadable modules are implemented:

- Load relocatable test module.
- Resolve exported symbol.
- Reject unresolved symbol.
- Reject ABI version mismatch.
- Call module init.
- Call module exit.
- Unload cleanly.
- Reject unload while references remain.

### Required markers

```text
OK_DRIVER_ABI native_probe=pass irq=pass mmio=pass remove=pass
OK_LINUX_DRIVER_SHIM compile=pass probe=pass mmio=pass alloc=pass remove=pass
OK_MODULE_LOAD elf=pass reloc=pass symbols=pass unload=pass
```

### Pass criteria

- A native OKernel driver and a Linux-shim driver can bind to the same fake bus/device model.
- Linux shim remains explicitly subset-based.
- Unsupported Linux kernel APIs fail at compile time with clear messages.
- No claim of general binary Linux driver compatibility is made.

---

# P7: Network and Storage Expansion

## Implementation Requirements

### Network

Implement:

- virtio-net driver
- Ethernet frame layer
- ARP
- IPv4 routing table
- ICMP echo
- UDP sockets
- TCP socket state machine expansion
- socket file descriptors
- `socket`
- `bind`
- `listen`
- `accept`
- `connect`
- `sendto`
- `recvfrom`
- `sendmsg`
- `recvmsg`
- `setsockopt`
- `getsockopt`
- `shutdown`

### Storage

Implement:

- block request queue
- block cache
- partition table parser
- filesystem mount from block device
- improved SimpleFS consistency checks
- EXT4 directory/inode read path
- read-only EXT4 file read
- writeback policy placeholder

## Test Requirements

### Network tests

- virtio-net device probe
- MAC address assignment
- ARP request/reply
- ICMP echo loopback
- UDP socket send/receive
- TCP local connect/listen/accept
- socket FD read/write path
- invalid socket operation errno mapping

### Storage tests

- block cache hit/miss counters
- MBR or GPT parse from test disk image
- mount filesystem from partition
- read file from EXT4 test image
- reject corrupted superblock
- reject out-of-range block reads

### Required markers

```text
OK_NETDEV virtio=pass arp=pass icmp=pass socket=pass
OK_SOCK udp=pass tcp=pass poll=pass
OK_BLOCK cache=pass partition=pass bounds=pass
OK_EXT4_READONLY super=pass inode=pass dir=pass file=pass
```

### Pass criteria

- Socket syscalls are backed by file descriptors.
- Network stack can be tested deterministically in QEMU.
- EXT4 support is honestly labelled read-only until write support exists.

---

# P8: SMP, Interrupts, and Preemptive Scheduling

## Implementation Requirements

### SMP

Implement real secondary CPU bring-up for x86_64 first.

Required features:

- ACPI RSDP/RSDT/XSDT parser
- MADT parser
- local APIC discovery
- IOAPIC discovery
- AP startup trampoline
- per-CPU data
- per-CPU stack
- per-CPU scheduler state
- CPU online handshake
- panic path if AP startup times out

### Interrupts

Implement:

- IDT setup
- exception handlers
- IRQ routing
- timer interrupt
- syscall entry
- page fault handler
- spurious interrupt handling
- interrupt nesting accounting

### Preemption

Implement:

- scheduler tick
- time slice
- preemption enable/disable counter
- context switch
- idle loop with halt
- wakeup from timer or IRQ
- per-CPU run queue mode

## Test Requirements

### SMP tests

Run QEMU with multiple CPUs:

```sh
qemu-system-x86_64 -smp 4 ...
```

Required checks:

- BSP online
- AP1/AP2/AP3 online
- per-CPU stack unique
- per-CPU data unique
- each CPU can run idle thread
- each CPU can schedule one test thread
- AP startup timeout is reported cleanly

### Interrupt tests

- divide-by-zero exception test in controlled mode
- page fault test in controlled mode
- timer IRQ increments tick counter
- syscall trap reaches dispatcher
- nested interrupt counter remains consistent

### Preemption tests

- two runnable threads alternate without voluntary yield
- blocked thread is not scheduled
- sleeping thread wakes after timer
- idle thread runs when no runnable tasks exist
- preemption disabled region does not switch

### Required markers

```text
OK_SMP cpus=4 online=4 ap_start=pass per_cpu=pass
OK_IRQ idt=pass timer=pass syscall=pass page_fault=pass
OK_PREEMPT tick=pass switch=pass sleep=pass idle=pass
```

### Pass criteria

- `xmake qemu-test -a x86_64` supports SMP test mode.
- SMP tests are deterministic or have clear timeout diagnostics.
- Single-core mode still works.

---

# Cross-Phase Test Matrix

## Minimum local validation before every commit

```sh
xmake f -c -m debug -a x86_64
xmake -y -b okernel
xmake -y -b okernel_image
xmake qemu-test
```

## Before merging a subsystem milestone

```sh
xmake profile-matrix
xmake qemu-matrix
```

## Before claiming architecture support

For the target architecture:

```sh
xmake f -c -m debug -a <arch>
xmake -y -b okernel
xmake -y -b okernel_image
xmake qemu-test -a <arch>
```

## Before claiming Linux syscall compatibility

Run:

```sh
xmake qemu-test -a x86_64
xmake linux-abi-test -a x86_64
```

The `linux-abi-test` task should eventually build and package tiny userland Linux-ABI smoke tests into the test root filesystem.

## Before claiming Linux driver compatibility

Run:

```sh
xmake qemu-test -a x86_64
xmake driver-abi-test -a x86_64
xmake linux-driver-shim-test -a x86_64
```

The `linux-driver-shim-test` task must compile at least one Linux-style test driver against the shim and validate that it binds to a fake or QEMU-visible device.

---

# Required Regression Markers

The final debug output should eventually include these marker groups as features land:

```text
OK_TEST_PASS arch=<arch> ...
OK_MODULES count=<n> failed=0
OK_VM kernel_map=pass user_map=pass user_copy=pass fault=pass
OK_PROC create=pass schedule=pass exit=pass wait=pass
OK_ELF load=pass entry=pass stack=pass
OK_USERLAND hello=pass fd=pass fork=pass exec=pass
OK_VFS mount=pass path=pass file=pass dir=pass symlink=pass
OK_DEVFS null=pass zero=pass console=pass
OK_PIPE create=pass transfer=pass poll=pass
OK_TTY console=pass ioctl=pass
OK_LINUX_ABI arch=<arch> args=pass errno=pass unknown=pass
OK_LINUX_SYSCALLS file=pass memory=pass time=pass futex=pass tls=pass
OK_DRIVER_ABI native_probe=pass irq=pass mmio=pass remove=pass
OK_LINUX_DRIVER_SHIM compile=pass probe=pass mmio=pass alloc=pass remove=pass
OK_MODULE_LOAD elf=pass reloc=pass symbols=pass unload=pass
OK_NETDEV virtio=pass arp=pass icmp=pass socket=pass
OK_SOCK udp=pass tcp=pass poll=pass
OK_BLOCK cache=pass partition=pass bounds=pass
OK_EXT4_READONLY super=pass inode=pass dir=pass file=pass
OK_SMP cpus=<n> online=<n> ap_start=pass per_cpu=pass
OK_IRQ idt=pass timer=pass syscall=pass page_fault=pass
OK_PREEMPT tick=pass switch=pass sleep=pass idle=pass
```

---

# Agent Task Template

Use this template for every implementation task.

```md
## Task: <short title>

### Goal

<what must be implemented>

### Files to inspect first

- <path>
- <path>

### Implementation steps

1. <step>
2. <step>
3. <step>

### Required tests

- <test>
- <test>

### Required commands

```sh
xmake f -c -m debug -a x86_64
xmake -y -b okernel
xmake qemu-test
```

### Required output markers

```text
OK_<MARKER> ...
```

### Definition of Done

- [ ] Builds with xmake.
- [ ] QEMU test passes.
- [ ] New debug test covers the feature.
- [ ] Failure path is tested.
- [ ] No existing marker regresses.
- [ ] Documentation is updated if a public contract changed.
```

---

# Definition of Done for the Whole Roadmap

The roadmap can be considered substantially complete when:

- The kernel boots through xmake/QEMU on the primary target.
- Core subsystems are managed as formal modules.
- User/kernel memory isolation exists.
- User programs can be loaded from ELF and can issue Linux-compatible syscalls.
- Basic UNIX process, FD, VFS, pipe, TTY, and device semantics exist.
- A minimal libc-like or Linux-ABI smoke-test userland runs.
- The driver system supports native OKernel drivers.
- A limited Linux source-compatibility shim can compile and run selected test drivers.
- The test matrix proves the above with deterministic QEMU markers.
