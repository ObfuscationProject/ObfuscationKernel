# Native MIPS execution

The `mips` and `mips64` profiles run user instructions on QEMU Malta with their
existing CPUs: default `24Kf` for MIPS32 and `MIPS64R2-generic` for MIPS64.
The backend uses KSU user mode, software-managed paired 4 KiB TLB entries and the
CP0 Count/Compare interrupt. It returns to the original kernel stack after each
syscall, timer interrupt or terminal fault. Kernel memory stays in privileged
KSEG0/KSEG1; address spaces have separate user mappings and flush stale TLB
entries when switching. Backing addresses are kernel KSEG0/KSEG1 pointers.

The shared implementation lives in `src/arch/mips_common/`; architecture-specific
translation units preserve the separate build profiles. Initialization enables
the ABI's floating-point mode before entering compiled C++: paired 32-bit FPRs
for O32, independent 64-bit FPRs for n64. A suspended frame preserves all ABI
integer registers, HI/LO, all floating-point registers, FCSR and TLS. Registers
`k0` and `k1` are kernel-reserved by the MIPS ABI. The assembly bridge also
preserves the kernel caller's callee-saved integer and floating-point state.
Execution remains on the boot CPU; this is not hardware SMP scheduling.

The native syscall convention is `v0` for the number and signed result and
registers `$4` through `$9` for six arguments. It uses Obfuscation UAPI numbers,
not the Linux MIPS syscall table or Linux's `a3` error flag. The OS O32 wrapper
moves later C arguments from stack offsets 16, 20 and 24 into the required
registers; n64 shifts the register arguments. The CRT interprets the native
word-sized argc/argv/envp stack. The n64 linker needs the explicit
`--oformat=elf64-bigmips` option with this toolchain.

The preserved CPUs do not expose CP0.UserLocal. The backend emulates
`rdhwr rt,$29` through the actual Reserved Instruction exception using the
suspended frame's `fs_base`. Syscalls and this TLS instruction work in ordinary
MIPS branch delay slots; the continuation decodes the branch before updating the
instruction result. Timer and memory exceptions keep EPC's hardware restart
semantics.

## Protection limits

Both preserved CPUs lack execute-inhibit/read-inhibit TLB bits. User/kernel
isolation, absent mappings and read-only writes are checked by the hardware.
The loader rejects writable executable mappings, and entry validation rejects
starting a frame in a data page, but user code can branch into a readable data
page and execute it. This is **not hardware NX protection**. The probe explicitly
executes a syscall from a writable data page to verify and expose that limitation.
Selecting a different CPU with RI/XI support would be a separate profile change.

## Hardware validation

```sh
python3 scripts/test_native_mips.py --arch mips
python3 scripts/test_native_mips.py --arch mips64
python3 scripts/test_native_mips.py --arch mips --kernel-fault
python3 scripts/test_native_mips.py --arch mips64 --kernel-fault
```

These independently compile the production backend and use real user-mode
instructions to check syscall/return, all ABI GPRs/FPRs/FCSR, HI/LO, timer resume,
TLB address-space switching, read-only/unmapped/kernel faults, TLS, branch-delay
continuation and the NX limitation. The optional final kernel fault checks that
an exception from privileged code reports a fatal kernel fault instead of
writing into a stale user frame. They do not substitute for the separate OS
ELF/CRT/libc and filesystem integration runs.
