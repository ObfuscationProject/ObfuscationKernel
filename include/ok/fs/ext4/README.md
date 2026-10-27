# ext4 module layout

The ext4 and JBD2 mechanisms live in eight module headers. Each module includes
only modules below it; `scripts/check_ext4_layout.py` enforces the layering, the
partial ownership below and module self-containment.

| Layer | Module | Contents | Includes |
| --- | --- | --- | --- |
| 1 | `journal.hpp` | `ok::fs::jbd2` record codec and replay planner, `ReplayExecutor`, `Ext4RecoveryBinding` | — |
| 2 | `layout.hpp` | `Ext4Volume`, timestamps, block accounting, `Ext4ReadOnlyView`, `Ext4FileSystem`/`Ext4Node` | journal |
| 3 | `extent.hpp` | first/next data plans, zero-length truncate and allocated-empty data plans, witnesses, previews and journal records | journal, layout |
| 4 | `alloc.hpp` | inode lifetime table, free-inode search, regular/directory create plans and witnesses, orphan chains, orphan reclaim plans and journal records, reclaim history | layout, extent |
| 5 | `dir.hpp` | directory initialization budget, namespace, rename and regular-move plans, witnesses and journal records | layout |
| 6 | `txn.hpp` | `Ext4ReplayPreview`, `Ext4RecoveryCompletion`, `MetadataBuffers`, `Ext4MetadataTransaction` and the per-mutation transaction writers | layout, extent, alloc, dir |
| 7 | `recovery.hpp` | replay shape, namespace and regular-move cold replay, recovery bindings and completion, reclaim capture, result and composed histories, orphan-chain capture and snapshot | journal, alloc, dir, txn |
| 8 | `mount.hpp` | mounted typed budgets and cursors, direct-read scope, startup workspace, `Ext4OwnedTransaction`, empty-orphan recovery, `Ext4MountedMetadata` | journal, alloc, dir, txn, recovery |

New code goes into the lowest module that already holds everything it uses. A
module that needs a higher one is a layering error, not a reason to reorder.

## SDK contract markers

`ok/fs/ext4_directory_create_sdk_profile.hpp` (included by `alloc.hpp`) and
`ok/fs/ext4_mounted_common_profile.hpp` (which includes `mount.hpp`) stay small,
unconditional marker headers like the other `*_profile.hpp` headers:
`tools/export_sdk.py` and the ObfuscationOS SDK consumer parse them with a strict
contract reader that refuses preprocessor conditionals and rejects the digit
separators the modules use.

## Private partials

`detail/*.inc` are partial class bodies, textually included inside one class
definition. They are exported with the SDK because the classes are templates, but
no file outside this directory includes them.

| Owner | Partials |
| --- | --- |
| `Ext4ZeroTruncateOwnershipValidator` (`extent.hpp`) | `completed_ownership_birth.inc` |
| `Ext4OwnedTransaction` (`mount.hpp`) | `owned_regular_move.inc`, `owned_typed_port.inc`, `owned_mounted_cursor.inc`, `owned_common_opening.inc`, `mounted_startup_port.inc` |
| `mounted_startup_port.inc` | `mounted_startup_locator.inc`, `mounted_reclaim_checkpoint.inc`, `mounted_startup_owner.inc`, `mounted_reclaim_history.inc`, `mounted_namespace_history.inc`, `mounted_namespace_checkpoint.inc`, `mounted_regular_move_startup.inc` |
| `Ext4MountedMetadata` (`mount.hpp`) | `mounted_original_node_authority.inc`, `mounted_typed_suffix.inc`, `mounted_typed_publication.inc`, `mounted_typed_request.inc`, `mounted_typed_cold_startup.inc`, `mounted_common_opening.inc`, `mounted_regular_move.inc` |

## Former per-slice headers

`refactor/ext4-layout` folded 85 headers from `include/ok/fs/` into the modules,
in dependency order and without changing a declaration, and moved the 20 partials
here without the `ext4_` prefix. Old include paths map as follows.

| Module | Former `ok/fs/<name>.hpp` |
| --- | --- |
| `journal.hpp` | `jbd2`, `jbd2_replay`, `ext4_recovery` |
| `layout.hpp` | `ext4`, `ext4_times`, `ext4_fs`, `ext4_blocks`, `ext4_readonly` |
| `extent.hpp` | `ext4_first_data_plan`, `ext4_next_data_plan`, `ext4_zero_truncate_plan`, `ext4_allocated_empty_data_plan`, `ext4_zero_truncate_witness`, `ext4_allocated_empty_data_ownership`, `ext4_allocated_empty_data_witness`, `ext4_zero_truncate_preview`, `ext4_allocated_empty_data_journal_verify`, `ext4_zero_truncate_journal`, `ext4_allocated_empty_data_journal`, `ext4_allocated_empty_data_preview`, `ext4_zero_truncate_transaction_budget`, `ext4_allocated_empty_data_transaction_budget` |
| `alloc.hpp` | `ext4_inode_lifetime`, `ext4_free_inode`, `ext4_create_plan`, `ext4_create_witness`, `ext4_directory_create_plan`, `ext4_directory_create_witness`, `ext4_orphan_chain`, `ext4_orphan_access`, `ext4_orphan_ownership`, `ext4_orphan_directory_refs`, `ext4_orphan_reclaim_plan`, `ext4_orphan_directory_qualified_reclaim`, `ext4_orphan_reclaim_witness`, `ext4_orphan_reclaim_journal`, `ext4_orphan_reclaim_cold`, `ext4_reclaim_history` |
| `dir.hpp` | `ext4_directory_initialization_budget`, `ext4_namespace_plan`, `ext4_rename_plan`, `ext4_regular_move_plan`, `ext4_regular_move_witness`, `ext4_namespace_journal`, `ext4_namespace_witness`, `ext4_regular_move_journal` |
| `txn.hpp` | `ext4_replay_view`, `ext4_completion`, `metadata_buffers`, `ext4_metadata_transaction`, `ext4_create_transaction`, `ext4_first_data_transaction`, `ext4_next_data_transaction`, `ext4_rename_transaction`, `ext4_namespace_transaction`, `ext4_regular_move_transaction`, `ext4_zero_truncate_session_preview`, `ext4_allocated_empty_data_session_preview`, `ext4_zero_truncate_transaction`, `ext4_allocated_empty_data_transaction` |
| `recovery.hpp` | `ext4_replay_shape`, `ext4_namespace_replay`, `ext4_namespace_cold_replay`, `ext4_regular_move_replay`, `ext4_regular_move_recovery`, `ext4_regular_move_completion`, `ext4_namespace_recovery_binding`, `ext4_namespace_completion`, `ext4_namespace_result_history`, `ext4_orphan_reclaim_capture`, `ext4_reclaim_recovery_binding`, `ext4_reclaim_result_history`, `ext4_reclaim_composed_budget`, `ext4_reclaim_route_budget`, `ext4_orphan_chain_capture`, `ext4_reclaim_composed_history`, `ext4_orphan_snapshot` |
| `mount.hpp` | `ext4_mounted_common_opening_budget`, `ext4_mounted_typed_budget`, `ext4_direct_read_scope`, `ext4_mounted_typed_cursor`, `ext4_mounted_startup_workspace`, `ext4_owned_transaction`, `ext4_empty_orphan_recovery`, `ext4_mounted_metadata` |
