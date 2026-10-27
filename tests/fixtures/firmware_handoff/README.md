# Typed firmware serial fixtures

These serial transcripts came from isolated QEMU discovery probes. They are
inputs for host-side acceptance-parser regression tests; replaying them is not
fresh QEMU execution or physical hardware qualification. The native runner
creates new source snapshots, images, serial transcripts and result receipts.

`loongarch64.txt` is QEMU 8.2, which passes no EFI system table to a direct ELF
kernel (`source_absent`). `loongarch64-efi.txt`, `mips.txt` and `mips64.txt`
come from the pinned QEMU 10.2.0 with the UEFI system table and Malta PROM
board contracts.
