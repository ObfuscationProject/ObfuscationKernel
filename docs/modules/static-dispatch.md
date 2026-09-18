# Static CPU operations and scheduling policy

The kernel is compiled for one ISA per build. All ten profiles remain supported:
`i386`, `x86_64`, `aarch64`, `arm32`, `rv64`, `rv32`, `loongarch64`, `mips`,
`mips64`, and `ppc`.

`ArchOperations` aliases `ArchitectureOperations<configured_architecture()>`.
Profile properties and context construction are visible to the compiler as
`constexpr` operations. Native instruction functions bind directly to the
`NativeInstructions<ISA>` specialization in that ISA's `src/arch/*/ops.cpp`.
There is no architecture-operation vtable. `ArchitectureInstructions` checks
the native backend contract; tests can supply a concrete recording backend.

Use `configured_arch_operations()` when retrieving the compiled CPU operations.
The compatibility `arch_operations(architecture)` accessor requires the argument
to match the compiled target and traps on an internal programming error.
Configuration boundaries should call `validate_architecture()` and report its
`unsupported` status before using the accessor. This interface cannot select a
foreign ISA at runtime.

`BasicScheduler<Policy>` similarly calls a concrete policy satisfying the
`SchedulerPolicy` concept. The default `Scheduler` alias uses `RoundRobinPolicy`.
The shared `SchedulerState` implementation owns control blocks and accounting;
the policy is stored by value, so copying a scheduler does not leave its policy
pointer referring into the original object.

Runtime registration remains appropriate for loadable modules, device drivers,
and their interrupt callbacks. This refactor does not erase those boundaries or
turn every object into a template.

The refactor changes dispatch and ownership, not implementation maturity.
`hardware_thread_count()` is still profile metadata, user contexts still describe
model state, and the scheduling mechanism does not execute a context switch.
No IDT setup, AP startup, MMU mapping, privilege transition, or preemption is
added by these templates.

Run `python3 scripts/test_static_dispatch.py` for the host checks. It compiles and
runs the same tests under each profile macro, with two different scheduling
policies and two recording architecture instantiations. The tests check all ten
profiles at compile time and reject a foreign process target before mutation.
Real target compilation and QEMU boot tests remain separate validation layers.

The next process refactor should merge `ProcessManager`'s `Process/Thread` and
the scheduler's `ProcessControlBlock/ThreadControlBlock` into one owned resource
model. A process should own credentials, descriptors and its address space;
threads should own execution context and enter run queues by stable identity.
That work should precede connecting fork/exec to real address-space switching.
