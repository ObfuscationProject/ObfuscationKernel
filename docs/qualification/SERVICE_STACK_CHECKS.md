# Service stack coordinates

The ten architecture boot sources export `ok_service_stack_lower` and
`ok_service_stack_top` at their existing `boot_stack` endpoints. These are aliases:
they allocate no storage and do not change the 1 MiB boot stack or its mappings.

`arch::read_instantaneous_stack_pointer()` reads the selected architecture's SP
register using constrained static dispatch. It requires an explicit native
freestanding profile, matching pointer size and byte order. The value includes
the current compiler frame. It is neither a frame-pointer approximation nor the
address of a local object.

`arch::StackBounds` checks ordered endpoints, containment and remaining bytes on
a downward-growing stack. The initial `SP == top` is valid. `SP == lower` has
zero remaining bytes. Bounds and suffix checks use subtraction after containment
validation, so a large requested suffix cannot wrap an address. These arithmetic
checks grant no lifetime or mapping authority.

The freestanding native runtime checks its actual SP against the linked service
stack at `start` and at the beginning of each `step`, before filesystem admission
or deferred owner cleanup. The zero-byte check enforces which stack executes the
service path; it is not a complete call-graph budget. Hosted runtime models skip
the native observation instead of reporting synthetic SPs as hardware evidence.

Run the isolated checks with:

```sh
python3 scripts/test_service_stack.py --output /tmp/service-stack-new-run
```

The script requires a fresh output directory and xmake. It uses the ten explicit
named roots in `toolchains/`, or an explicit `--toolchains` directory, without
SDK artifact selection or fallback globbing. It retains source and tool hashes,
commands, logs, exact product paths and separate result rows. It builds hosted
boundary tests with and without ASan/UBSan, compiles and links the actual boot
aliases and native SP probe on all ten profiles with Debug/Release optimization
flags, and runs the two x86_64 probes in QEMU. The nested probe keeps a real local
frame live and checks that the observed SP descends while remaining within the
actual linked boot stack. Other architecture rows establish compilation and
linkage only.

This is a minimal boot fixture, not a complete Kernel boot or an OS SDK-consumer
test. It does not qualify supervisor-only mappings, faulting guard pages,
interrupt masks, CPU identity/epochs, parked continuations, stack-domain switches,
IRQ/fault nesting, complete stack-usage bounds, SMP or physical hardware. The
entry-stack pool's mapped canaries remain diagnostic storage. The ext4 pair stays
disabled until its separate lifetime, mapping and linked-stack gates pass.
