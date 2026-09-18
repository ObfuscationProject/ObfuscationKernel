# loongarch64

The `loongarch64` profile provides native PLV3 execution on QEMU virt, alongside
the architecture-neutral model tests.

## Boot and platform

- `boot.S` enables scalar floating point, establishes the bootstrap stack,
  clears `.bss` and enters the kernel.
- `platform.cpp` provides NS16550 serial and RAM framebuffer support.
- `linker.ld` places the kernel ELF at `0x90000000`.
- `ops.cpp` supplies statically selected architecture instructions.

The freestanding build uses `-msimd=none`; LSX/LASX are not enabled.

## Native execution

`execution.cpp` and `execution.S` implement the common native execution API:

- The kernel remains at PLV0 with direct physical addressing between user
  entries. DMW0 permits the same identity addresses at PLV0 during exceptions;
  PLV3 cannot use that window.
- User virtual addresses occupy `[0x40000000, 0x80000000)`. Software TLB refill
  installs paired 4 KiB mappings with PLV3, read-only and execute-never bits.
  User backing pages remain caller-owned; the backend keeps up to 128 mappings
  per space and 16 generation-checked address-space handles.
- Real `ertn` entry, `syscall` return, architectural timer preemption and
  exception/refill vectors. The generic runtime receives traps synchronously
  on its original kernel stack and owns process policy.
- General registers, scalar floating-point registers, FCSR, FCC and thread
  pointer state are preserved. Kernel TLS and FP control state are restored
  before compiled trap handling. The canonical `UserFrame.sp` is the stack
  pointer validated and installed on entry.
- Exception vectors check prior privilege before touching a user-frame
  pointer or either stack. A PLV0 fault reports a kernel fault and halts;
  scratch CSRs KS0/KS1 are restored after each user entry.
- User copies validate all requested pages and use kernel physical aliases.

Only one CPU executes native processes. Hardware SMP, general physical-memory
allocation, LSX/LASX state and device IRQ integration remain future work.

## Hardware acceptance tests

From the kernel repository:

```sh
python3 scripts/test_native_loongarch64.py
python3 scripts/test_native_loongarch64.py --kernel-fault --output build/native-tests/loongarch64-kernel-fault
```

The independent build does not change xmake configuration. The normal probe
executes PLV3 syscall/return, timer preemption, read-only protection, kernel
address isolation, alternating hardware address spaces, FP/FCC/TLS preservation,
safe copies, stale handles and invalid instructions. It also checks canonical
SP and kernel scratch-register restoration. The second command deliberately
raises a PLV0 exception with an invalid KS0 pointer and requires the safe
`OK_NATIVE_KERNEL_FAULT loongarch64` diagnostic. QEMU halts at the final marker
and is terminated by the test runner's timeout.

The default `noexecute-entry-guard` check validates the software entry guard;
it does **not** claim hardware NX exception recovery. A separate strict probe
requires an actual branch from executable code into a mapped NX data page:

```sh
python3 scripts/test_native_loongarch64.py --nx-branch --output build/native-tests/loongarch64-nx
```

On the tested QEMU 8.2.2 this probe fails because exception delivery recursively
fetches the faulting NX instruction before entering EENTRY. In upstream
[`target/loongarch/cpu.c`, lines 203-214](https://github.com/qemu/qemu/blob/v8.2.2/target/loongarch/cpu.c#L203),
`EXCCODE_PNX` leaves `update_badinstr` enabled and `cpu_ldl_code(env, env->pc)`
raises the same NX exception again, before the emulator changes privilege or
PC. This matches the observed repeated `Page Not Executable` messages with
unchanged PC/ERA and no kernel vector entry. The same source pattern exists
in upstream tags v9.0.0, v9.2.0 and v10.0.0; changing versions alone is not a
verified remedy.

NX remains set on non-executable hardware mappings. No writable page is made
executable to bypass the emulator defect. Until a corrected emulator or real
hardware passes the strict probe, LoongArch hardware NX fault recovery is an
explicit acceptance gap: a user branch into such a page can stall this QEMU
version before the kernel can terminate the process.
