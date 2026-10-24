# Supervisor object page isolation probe

`tests/native/object_isolation_probe.cpp` runs the real page provider and object
pool after the native backend establishes supervisor mappings. It retains one
live object, maps user-purpose code/data/stack pages, and enters user privilege.
A user load of a valid user pointer must complete through syscall 65. The same
load instruction then targets the actual supervisor object address and must
fault with the exact address and PC, leaving the user output sentinel unchanged.
A final positive load must succeed. Only successful address-space retirement
permits releasing the three user pages and destroying/trimming the object.

All ten architecture implementations are preserved. i386, x86_64, ARM32,
AArch64, RV32, RV64 and PPC use qualified firmware RAM. MIPS, MIPS64 and
LoongArch64 use explicit linker-reserved backing; MIPS supervisor aliases are
preserved. This probe does not make their firmware RAM discovery complete.

Build and execute the isolated Kernel fixture with xmake:

```sh
python3 -B scripts/test_native_object_isolation.py \
  --toolchains /absolute/path/to/toolchains \
  --output /absolute/path/to/new-qualification-directory
```

An optional `--arch` selects a row, without changing the default ten-profile
matrix. Sources, generated xmake recipes, tool identities, ELF/image files,
commands and raw serial are retained for every attempted row. The parser binds
fault and continuation PCs to the three symbols in the retained ELF, checks
source-specific backing ranges and accounting, and rejects incomplete records.
The current harness does not yet inventory all transitive compiler toolchain
headers/runtime inputs; do not label its receipt a complete reproducible-input
closure. It is separate from an OS SDK-consumer or libc integration test.

`tests/host/object_isolation_evidence_tests.py` carries the independent review's
corruption corpus with ten raw samples. It exercises 361 valid/corrupt records
and additional layout refusals. Those samples are parser fixtures, not fresh
hardware executions. The original V4 QEMU run passed all ten rows; a subsequent
parser-only repair passed the same retained ELF/serial inputs and corruption
controls. Original failed build attempts and parser findings remain retained.

The evidence is scoped to serialized boot-CPU user loads of these live object
pages, fault return, recovery and reclamation. It does not establish physical
hardware support, IRQ/SMP synchronization, general process isolation, DMA
retirement or completion of M1/M0-M12.
