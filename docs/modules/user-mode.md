# User Mode

`ok::user::UserSpaceManager` delegates transitions to `UserModeGateway`. The
baseline gateway is simulated and updates a CPU context to user mode.

`TransitionMode` keeps architecture entry/return variants visible:

- `simulated`: debug and freestanding profile transition state.
- `trap_return`: return-from-interrupt or return-from-exception path.
- `fast_return`: architecture fast-return path such as `sysret`-style flows.

Real architecture implementations will replace this with `iret`, `sysret`,
`eret`, `sret`, or equivalent return-from-exception sequences.

The debug kernel validates the user-mode manager through the same `kernel_main`
path as normal boots. POSIX file and filesystem tests currently run inside the
kernel debug harness; later user ABI tests can reuse the same syscall table and
POSIX service.

For downstream 0.1.x user-space experiments, the public C header is
`include/ok/uapi/syscall.h`. It describes the syscall and native ABI constants
that are stable before the architecture-specific trap-return paths are ready.
The current gateway still simulates user entry, so a real user ELF loader,
crt0/sysroot, page-table isolation, and return-from-trap implementation remain
release blockers for running arbitrary user binaries as isolated programs.
