# Qualification notes

Per-slice qualification notes for ObfuscationKernel. Each file records one bounded
implementation slice, its exact scope, its tests and its limits at the time it was
qualified. They are historical records: the subsystem documents under `../modules/`
and the top-level design documents describe current behavior, and the ledger-driven
`ObfuscationOS/docs/plans/STATUS.md` is the capability matrix. Execution receipts
live in the separate ObfuscationEvidence repository.

| File | Subject |
| --- | --- |
| [ACCESS_PERMISSION_QUERIES.md](ACCESS_PERMISSION_QUERIES.md) | Access permission queries |
| [ALLOCATED_EMPTY_RECOVERY_PREVIEW.md](ALLOCATED_EMPTY_RECOVERY_PREVIEW.md) | Private allocated-empty recovery preview |
| [ALLOCATED_EMPTY_SESSION_PREVIEW.md](ALLOCATED_EMPTY_SESSION_PREVIEW.md) | Guarded allocated-empty Session preview |
| [BOOT_MEMORY_HANDOFF.md](BOOT_MEMORY_HANDOFF.md) | Boot memory parsing and remaining handoff work |
| [BOOT_MEMORY_MAP.md](BOOT_MEMORY_MAP.md) | Discovered boot memory map on ten profiles |
| [CHECKED_CHILD_REAP.md](CHECKED_CHILD_REAP.md) | Checked child detachment and orphan reaping |
| [CHECKED_VFS_CALLERS.md](CHECKED_VFS_CALLERS.md) | Checked filesystem callers candidate |
| [EXT4_ALLOCATED_EMPTY_DATA.md](EXT4_ALLOCATED_EMPTY_DATA.md) | Private allocated-empty first-block planner |
| [EXT4_MODULE_LAYOUT.md](EXT4_MODULE_LAYOUT.md) | ext4 module header layout and equivalence checks |
| [EXT4_MOUNTED_METADATA.md](EXT4_MOUNTED_METADATA.md) | Bounded mounted ext4 metadata policies |
| [EXT4_ZERO_TRUNCATE_SLICE1.md](EXT4_ZERO_TRUNCATE_SLICE1.md) | Zero-truncate source slice 1 |
| [EXT4_ZERO_TRUNCATE_SLICE2.md](EXT4_ZERO_TRUNCATE_SLICE2.md) | Exact linked-inode zero-truncate witness |
| [EXT4_ZERO_TRUNCATE_SLICE3.md](EXT4_ZERO_TRUNCATE_SLICE3.md) | Committed zero-truncate journal and ownership preview |
| [EXT4_ZERO_TRUNCATE_SLICE4.md](EXT4_ZERO_TRUNCATE_SLICE4.md) | Zero-truncate journal formatting and guarded Session preview |
| [EXT4_ZERO_TRUNCATE_TRANSACTION.md](EXT4_ZERO_TRUNCATE_TRANSACTION.md) | Private zero-truncate transaction |
| [INITIAL_NODE_ATTRIBUTES.md](INITIAL_NODE_ATTRIBUTES.md) | Initial node attributes and namespace publication |
| [LEGACY_ACTIVE_STARTUP.md](LEGACY_ACTIVE_STARTUP.md) | Legacy journal recovery with startup storage |
| [LINUX_BOOTSTRAP_ADAPTER.md](LINUX_BOOTSTRAP_ADAPTER.md) | Bounded Linux bootstrap adapter |
| [LINUX_CHMOD.md](LINUX_CHMOD.md) | Checked Linux mode changes (isolated prototype) |
| [LINUX_DESCRIPTOR_BOOTSTRAP.md](LINUX_DESCRIPTOR_BOOTSTRAP.md) | Linux descriptor bootstrap |
| [LINUX_DIRECTORY_STREAMS.md](LINUX_DIRECTORY_STREAMS.md) | Bounded Linux directory streams |
| [LINUX_EXEC_BOOTSTRAP.md](LINUX_EXEC_BOOTSTRAP.md) | Linux static ELF bootstrap metadata |
| [LINUX_MKDIR.md](LINUX_MKDIR.md) | Linux directory creation |
| [LINUX_PATH_BOOTSTRAP.md](LINUX_PATH_BOOTSTRAP.md) | Linux pathname bootstrap calls |
| [LINUX_POSITIONAL_IO.md](LINUX_POSITIONAL_IO.md) | Bounded Linux positional I/O |
| [LINUX_STAT.md](LINUX_STAT.md) | Qualified Linux stat subset |
| [LINUX_UMASK.md](LINUX_UMASK.md) | Linux process creation mask |
| [MAINTAINED_EXT4_TIMESTAMPS.md](MAINTAINED_EXT4_TIMESTAMPS.md) | Explicit maintained ext4 timestamps |
| [MOUNTED_MUTATION_IDENTITY.md](MOUNTED_MUTATION_IDENTITY.md) | Authentic Node admission for mounted write and truncate |
| [MOUNT_RETENTION.md](MOUNT_RETENTION.md) | Owned mount publication and resource drainage |
| [NATIVE_CHILD_EXIT_SIGNALS.md](NATIVE_CHILD_EXIT_SIGNALS.md) | Private native child exit signals |
| [NATIVE_CHILD_SIGINFO.md](NATIVE_CHILD_SIGINFO.md) | Typed child siginfo prerequisite |
| [NATIVE_CHILD_WAIT_SIGNALS.md](NATIVE_CHILD_WAIT_SIGNALS.md) | Owned native child waits and private caught signals |
| [NATIVE_CONSOLE_CONTROL.md](NATIVE_CONSOLE_CONTROL.md) | Bounded controlling console and foreground groups |
| [NATIVE_CONSOLE_INPUT.md](NATIVE_CONSOLE_INPUT.md) | Native console input lease |
| [NATIVE_COW_FORK.md](NATIVE_COW_FORK.md) | Copy-on-write native fork |
| [NATIVE_CREDENTIALS.md](NATIVE_CREDENTIALS.md) | Native Process Credentials |
| [NATIVE_DYNAMIC_STARTUP.md](NATIVE_DYNAMIC_STARTUP.md) | First x86-64 shared-musl startup candidate |
| [NATIVE_ENTROPY.md](NATIVE_ENTROPY.md) | Trusted entropy and Linux initial-stack randomness |
| [NATIVE_ENTRY_STACKS.md](NATIVE_ENTRY_STACKS.md) | Native kernel entry stack ownership (SDK26) |
| [NATIVE_INIT_ADOPTION.md](NATIVE_INIT_ADOPTION.md) | Native and Linux PID 1 adoption |
| [NATIVE_JOURNALLED_CREATE.md](NATIVE_JOURNALLED_CREATE.md) | Bounded journalled empty-file creation |
| [NATIVE_JOURNALLED_FIRST_DATA.md](NATIVE_JOURNALLED_FIRST_DATA.md) | Bounded journalled first-data allocation |
| [NATIVE_METADATA.md](NATIVE_METADATA.md) | Native checked file metadata |
| [NATIVE_MOUNT_INFO.md](NATIVE_MOUNT_INFO.md) | Read-only native mount inventory |
| [NATIVE_OBJECT_PAGES.md](NATIVE_OBJECT_PAGES.md) | Native user and Kernel object page ownership |
| [NATIVE_PERSISTENT_SYNC.md](NATIVE_PERSISTENT_SYNC.md) | Strict persistent fd synchronization |
| [NATIVE_PIPE_BLOCKING_WRITES.md](NATIVE_PIPE_BLOCKING_WRITES.md) | Complete serialized blocking pipe writes |
| [NATIVE_PIPE_READS.md](NATIVE_PIPE_READS.md) | Owned scalar pipe read continuations |
| [NATIVE_PIPE_SIGPIPE.md](NATIVE_PIPE_SIGPIPE.md) | Private automatic pipe SIGPIPE |
| [NATIVE_PIPE_VECTORS.md](NATIVE_PIPE_VECTORS.md) | Owned pipe readv/writev continuations |
| [NATIVE_PIPE_WRITE_SIGNALS.md](NATIVE_PIPE_WRITE_SIGNALS.md) | Owned scalar pipe writes and private caught signals |
| [NATIVE_POLL_OBJECTS.md](NATIVE_POLL_OBJECTS.md) | Native poll objects on owned pages |
| [NATIVE_POLL_TEMPORARY_MASKS.md](NATIVE_POLL_TEMPORARY_MASKS.md) | Private ppoll and pselect temporary signal masks |
| [NATIVE_POWER.md](NATIVE_POWER.md) | Native power reservation and queued commit |
| [NATIVE_PROCESS_FRAMES.md](NATIVE_PROCESS_FRAMES.md) | Bounded native process frame ownership |
| [NATIVE_PROCESS_GROUP_SIGNALS.md](NATIVE_PROCESS_GROUP_SIGNALS.md) | Bounded process-group signal selection |
| [NATIVE_PROCESS_SESSIONS.md](NATIVE_PROCESS_SESSIONS.md) | Native process sessions and process groups |
| [NATIVE_PROCESS_SIGNALS.md](NATIVE_PROCESS_SIGNALS.md) | Private positive-target process signals |
| [NATIVE_PROCESS_TERMINATION.md](NATIVE_PROCESS_TERMINATION.md) | Native process termination |
| [NATIVE_RESTART_PUBLICATION.md](NATIVE_RESTART_PUBLICATION.md) | Owned restart publication prerequisite |
| [NATIVE_ROBUST_LISTS.md](NATIVE_ROBUST_LISTS.md) | Serialized native robust-list cleanup |
| [NATIVE_SIGNAL_MASKS.md](NATIVE_SIGNAL_MASKS.md) | Native per-thread Linux signal masks |
| [NATIVE_SIGNAL_PRODUCTION.md](NATIVE_SIGNAL_PRODUCTION.md) | Ordinary bounded native signal runtime |
| [NATIVE_SIGNAL_RUNTIME.md](NATIVE_SIGNAL_RUNTIME.md) | Native signal runtime ownership |
| [NATIVE_SIGNAL_WAIT.md](NATIVE_SIGNAL_WAIT.md) | Private synchronous signal waits |
| [NATIVE_STAT_IDENTITY.md](NATIVE_STAT_IDENTITY.md) | Native file identity |
| [NATIVE_SYSCALL_ORIGINS.md](NATIVE_SYSCALL_ORIGINS.md) | Owned native syscall origins |
| [NATIVE_SYSCALL_RESTART_ADAPTER.md](NATIVE_SYSCALL_RESTART_ADAPTER.md) | Static syscall restart preparation |
| [NATIVE_TABLE_STORAGE.md](NATIVE_TABLE_STORAGE.md) | Runtime-allocated address spaces, mapping records and shared files |
| [NATIVE_VM.md](NATIVE_VM.md) | Bounded native virtual memory |
| [NATIVE_WAITS.md](NATIVE_WAITS.md) | Native waits and descriptor readiness |
| [NETWORK_WIRE.md](NETWORK_WIRE.md) | Network wire bytes |
| [OBJECT_PAGE_ISOLATION.md](OBJECT_PAGE_ISOLATION.md) | Supervisor object page isolation probe |
| [OBJECT_POOL.md](OBJECT_POOL.md) | Bounded physical-page-backed object pool |
| [PENDING_SIGNALS.md](PENDING_SIGNALS.md) | Internal pending signal queue |
| [PLATFORM_CLOCKS.md](PLATFORM_CLOCKS.md) | Native platform clocks |
| [PLATFORM_INTERRUPTS.md](PLATFORM_INTERRUPTS.md) | Platform interrupt lines, the top/bottom-half router and the idle wait (WT1.2) |
| [PRIVATE_FUTEX.md](PRIVATE_FUTEX.md) | Private futex implementation contract |
| [PRIVATE_FUTEX_PEER_FIXTURE.md](PRIVATE_FUTEX_PEER_FIXTURE.md) | Actual private futex peer execution fixture |
| [PROCESS_MEMORY_QUOTAS.md](PROCESS_MEMORY_QUOTAS.md) | Per-process memory quotas |
| [PUBLIC_REALTIME.md](PUBLIC_REALTIME.md) | Checked public realtime clocks |
| [QEMU_RUNTIME_FRAMES.md](QEMU_RUNTIME_FRAMES.md) | Qualified QEMU runtime frame bridge |
| [QUALIFIED_BOOT_MEMORY.md](QUALIFIED_BOOT_MEMORY.md) | Qualified entry memory |
| [QUALIFIED_REALTIME.md](QUALIFIED_REALTIME.md) | Qualified epoch mechanisms and platform diagnostics |
| [REALTIME_SETTING.md](REALTIME_SETTING.md) | Explicit administrative realtime setting |
| [SAVED_CREDENTIALS.md](SAVED_CREDENTIALS.md) | Saved user and group IDs |
| [SERVICE_STACK_CHECKS.md](SERVICE_STACK_CHECKS.md) | Service stack coordinates |
| [SHUTDOWN_COORDINATOR.md](SHUTDOWN_COORDINATOR.md) | Shutdown coordinator candidate |
| [THREAD_LIFECYCLE.md](THREAD_LIFECYCLE.md) | Serialized native thread foundation (SDK26) |
| [TYPED_FIRMWARE_HANDOFF.md](TYPED_FIRMWARE_HANDOFF.md) | Typed firmware memory discovery |
| [UDP_ZERO_DESTINATION_SEND.md](UDP_ZERO_DESTINATION_SEND.md) | Connected UDP zero-destination send successor |
| [USER_RUNTIME_SPLIT.md](USER_RUNTIME_SPLIT.md) | Native runtime split, signal headers and rescue gate |
| [WAIT_REGISTRATION.md](WAIT_REGISTRATION.md) | Serialized wait registration prototype |
