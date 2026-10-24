# ext4 module layout

Target of `refactor/ext4-layout`: eight module headers under `include/ok/fs/ext4/`
replace the 100-plus per-slice headers in `include/ok/fs/`. The facades exist now
and include the slices they will absorb; `.inc` files are private partial class
bodies and are never included by a facade.

| Module | Purpose | Slices |
| --- | --- | --- |
| `layout.hpp` | On-disk layout, superblock/group descriptors, read-only access, timestamps | 5 |
| `extent.hpp` | Extent-backed data blocks: first/next data, allocated-empty data, zero-length truncate | 19 |
| `dir.hpp` | Directory entries, namespace mutation, rename and regular-file moves | 12 |
| `alloc.hpp` | Inode/block allocation, creation plans, orphan chains and reclaim | 24 |
| `txn.hpp` | Metadata transactions, plans, witnesses, completion, owned transactions, metadata buffers | 4 |
| `journal.hpp` | JBD2 descriptor/replay model and journalled variants of the mutation slices | 13 |
| `recovery.hpp` | Recovery bindings and cold replay after interrupted mutation | 5 |
| `mount.hpp` | Mounted-instance startup, common opening, typed cursors and retention | 6 |

Private `.inc` partials (20): `ext4_completed_ownership_birth.inc`, `ext4_mounted_common_opening.inc`, `ext4_mounted_namespace_checkpoint.inc`, `ext4_mounted_namespace_history.inc`, `ext4_mounted_original_node_authority.inc`, `ext4_mounted_reclaim_checkpoint.inc`, `ext4_mounted_reclaim_history.inc`, `ext4_mounted_regular_move.inc`, `ext4_mounted_regular_move_startup.inc`, `ext4_mounted_startup_locator.inc`, `ext4_mounted_startup_owner.inc`, `ext4_mounted_startup_port.inc`, `ext4_mounted_typed_cold_startup.inc`, `ext4_mounted_typed_publication.inc`, `ext4_mounted_typed_request.inc`, `ext4_mounted_typed_suffix.inc`, `ext4_owned_common_opening.inc`, `ext4_owned_mounted_cursor.inc`, `ext4_owned_regular_move.inc`, `ext4_owned_typed_port.inc`

Known include-order dependency (`scripts/check_ext4_layout.py`): `ext4_regular_move_completion.hpp`
is not self-contained; `--strict` fails until it includes what it uses.

Migration steps: (1) facades include slices [done]; (2) `src/fs/*.cpp` and tests include
facades only; (3) fold each slice into its facade and delete it; (4) move `.inc` partials
under `src/fs/ext4/`; (5) bump the module ABI once at merge. Behaviour must not change:
the hosted ext4 suites and the OS two-boot persistence test are the oracle.
