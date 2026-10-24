# Private allocated-empty recovery preview

`Ext4AllocatedEmptyDataPreview<Reader>` is a read-only, permanently one-shot
preview of a bounded allocated-empty first-block transaction. It captures the
complete committed eight-record journal, unchanged inode bitmap, four physical
homes and candidate data block. Its canonical witness distinguishes all sixteen
exact old/after home combinations. Any third whole-home image is refused.

The old overlay proves the exact original allocated, linked empty inode, its
version, global allocation ownership and free candidate through
`Ext4AllocatedEmptyDataOwnershipValidator`. The final overlay proves complete
global ownership and the exact one-block mapping, data group, inode generation
and proposed size. Both overlays substitute only the four canonical homes.
Every other read uses the same retained reader. Fourteen complete physical
readbacks must match the captured images before the report becomes ready.

Preview and workspace objects are noncopyable and nonmovable. Allocate the large
workspace outside kernel stacks. The reader, its reachable backing storage and
the binding must outlive the attempt, remain exclusive and stay disjoint from
the mutable workspace. Direct object/input overlap is refused before clearing
or dispatch; arbitrary storage reachable through an opaque reader is a caller
obligation. A workspace claim is never released on success or failure. A second
call, even from a new preview, cannot reuse it. Report copies supply diagnostics
and cannot authorize raw I/O or replace the retained private history.

Data capture/readback proves only byte consistency. There is no data checksum
in the metadata witness. The later live writer must write and successfully
FLUSH a complete data image before publishing its metadata journal. This preview
does not manufacture data durability, writer exclusion, Session counter
reservations, device/partition lifetimes or mounted cache publication.

The proposed composed physical READ capacity is `2876 + 1062G`: two inherited
strict opening/global ownership bounds (`1420 + 531G` each), eight additional
allocated-empty ownership metadata reads and fourteen capture/readback pairs.
Exact Session adaptation must derive and reserve the complete route before its
first dispatch. Ordinary and ASan/UBSan hosted controls cover all sixteen
mixtures at file sizes 1/73/1024, all exercised one/two-group READ failure
positions, partial reads, fourteen changed bookends, permanent claims, direct
aliases, bounded parser refusal and a CRC-valid cross-group ownership conflict.
These models do not qualify target stack chains, actual device execution, mounted
recovery, the exported SDK or full M4/M0-M12.
