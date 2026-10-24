# Actual private futex peer execution fixture

The opt-in Debug fixture runs a raw Linux-personality ELF through the ordinary
native loader, hardware entry/trap backend and runtime `step()`. It creates four
TCBs sharing the loaded address space, each using a disjoint 16 KiB slice of the
loader's 64 KiB stack. These are harness-created user threads, not clone-created
threads or musl pthreads. No production thread-creation interface is added.

The first three roles issue real `FUTEX_WAIT_PRIVATE` traps. The peer stays
blocked until the kernel has observed all three exact pending tickets. It then
publishes word=99 with release ordering and issues WAKE counts 0, 1 and -1. Each
must wake one; each waiter must return zero and acquire the changed user word.
All roles then park on a distinct control word. Four explicitly counted harness
control completions start a second round. The second round checks increasing
tickets and the invalidity of retired word handles. No guessed delay is used.
The test retires the entire group after both rounds; it does not test thread
exit, clone, TLS, robust owner death, shared futex, PI, signals or SMP.

The fixture is disabled by default. `--private_futex_peer_fixture=y` enables it
only in Debug builds and records `OK_PRIVATE_FUTEX_PEER_FIXTURE` in the exported
SDK defines. Normal Debug and Release builds do not install peers or run these
checks. A fixture SDK requires the raw peer ELF as `/init`; it is a diagnostic
artifact and is not a general distribution image.

Build the producer with xmake, explicitly choosing architecture, Debug mode and
toolchain, then export `kernel-sdk`. Run standalone `scripts/qemu_test.py` against
that exact SDK image. Build the raw ELF with:

```sh
python3 scripts/build_private_futex_peer.py --arch x86_64 \
  --toolchain /absolute/toolchain --out /absolute/new-elf-output
```

The builder preserves all ten profiles and uses freestanding C++23, enabled
RTTI, target assembly and xmake. The emitted raw and Linux-marked ELF files and
source/compiler/command hashes are retained. A successful build is a compilation
row, not an execution row.

Freeze the chosen OS source with its existing
`tools/qualify_private_futex.py freeze --output /absolute/inventory.json` command.
Then run `scripts/qualify_private_futex_peers.py` with explicit architecture,
OS source and inventory, SDK, ELF, compiler toolchain, QEMU, fresh output path,
and the required inventory/SDK/ELF/compiler/QEMU SHA-256 values. The qualifier
copies the admitted OS sources and exact SDK into its output, builds a fresh
SDK-only distribution consumer, installs the raw ELF as `/init` on a new ext4
image, and runs QEMU. It never compiles Kernel source from the OS build.

Acceptance requires the exact setup, two admission records, two completed
rounds, 20 actual user syscall-trap records involving four distinct TIDs, and
one final pass record. Six traps are real user WAKE calls; eight are user parks;
four completions are privileged harness control actions. The final record
requires object/index/page/pin/pending cleanup. Parser controls reject missing,
duplicated, replayed, mis-keyed or model-only evidence. Tests use the retained
x86_64 serial excerpt only as parser input; re-reading it is not a new guest run.

The initial accepted execution profile is x86_64 QEMU. Other profiles remain
visible as compilation-only until their own producer, SDK-only consumer and
actual peer-execution receipts pass. QEMU native user execution does not establish
physical hardware qualification, pthread compatibility or complete M3/P07.
