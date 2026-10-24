# Ordered directory free-block initialization

This private ABI64 route extends the bounded 1 KiB directory creation owner.
The strict planner, directory witness and cold replay still require a zero free
block preimage. Neither candidate admission nor metadata parsing loads code.

Read-only preparation selects the first initialized-group free inode and data
block, proves that no allocated or reserved inode references the data block,
freezes the old journal and all allocation/namespace participants, and reserves
canonical pins. A nonzero candidate keeps old images staged without a directory
witness. Preparation performs no write or flush. Both the format probe and the
actual inspected binding require 1 KiB blocks before nested view validation.

Commit retains the original Owner, writable Session, transaction, cache slots,
pins and counter history. It repeats the physical old-image and ownership proof,
then bookends the frozen journal and participants again after that scan. A
nonzero candidate receives one zero block write and one successful data flush.
Direct physical readback must prove zero; the immutable participants and the
ownership proof are checked again. Only then does the existing strict planner
construct the canonical directory and witness, followed by proposed replay.
Another physical journal/all-home bookend precedes journal publication.

The cache changes only the proved-unallocated data canonical preimage to zero
and advances that retained pin generation before staging final images. Metadata
pins and the transaction/session epochs survive. No spinlock is held during
transport I/O. Every failed attempted initialization write, flush, readback or
later endpoint proof quarantines the original owner/session. A failed zeroing
attempt cannot publish namespace, allocation metadata or journal intent.

Private binding/counter guards reserve all six attempted/completed headrooms
before preparation reads, require completed parity and exact endpoints, and
admit bounded read-only, initialization, journal and checkpoint deltas. Only
proved drained rearm resets the local guard; Session counter history never
restarts. The arithmetic includes both the already-zero and nonzero branches;
see directory-initialization-budget.md. Mounted verification is accounted before
rearm and requires the actual 1 KiB binding before directory callbacks.

The hosted controls use the original stale-data fixture without changing that
file. Explicit in-memory free-inode and zero-path variants are model fixtures.
They cover public Owner and mounted APIs, strict zero compatibility, valid-CRC
participant drift, a late ownership-scan drift bookend, Session endpoint drift,
initialization partial cuts, after-effect failures, readback refusal and later
old-or-complete replay. Their receipts do not establish hardware power-loss,
userland execution, isolation or native media sanitation. Root alone executes
the xmake/compiler/linker/archive/model and native qualification recipes.

This source does not compose the separately qualified directory-rename repair
or cross-parent regular-file movement. Those remain distinct successors before
the complete Files workflow can be executed. All ten Kernel architectures,
twenty architecture/mode rows and four supplied musl profiles remain required.
The OS consumes exported architecture/mode-specific Kernel SDK artifacts.
