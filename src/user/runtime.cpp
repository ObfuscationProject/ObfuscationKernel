// Native runtime service state, diagnostics, thread retirement, result encoding,
// process completion and shutdown. The units are mapped in src/user/README.md.
#include "runtime/private.hpp"

namespace ok::user::native
{
inline namespace runtime_detail
{
static_assert(native_sessions_module_abi == kernel_module_abi_version);
static_assert(native_sessions_version == OK_NATIVE_SESSIONS_VERSION);
static_assert(native_sessions_process_capacity == sched::max_processes);
static_assert(native_console_control_module_abi == kernel_module_abi_version);
static_assert(native_console_control_version == OK_NATIVE_CONSOLE_CONTROL_VERSION);
static_assert(native_console_control_pid_bytes == 4 && native_sessions_controlling_tty == 1 &&
              native_group_signal_controlling_tty == 1 && native_console_control_pty == 0 &&
              native_console_control_termios == 0 && native_console_control_job_control == 0 &&
              native_console_control_smp == 0);
static_assert(native_group_signal_module_abi == kernel_module_abi_version);
static_assert(native_group_signal_process_capacity == sched::max_processes);
static_assert(native_group_signal_profile == "bootcpu-signed32-current-negative-pgid-probe-term-kill-deferred-partial-v2");
static_assert(native_group_signal_current_group == 1 && native_group_signal_negative_group == 1 &&
              native_group_signal_probe == 1 && native_group_signal_term == OK_SIGTERM &&
              native_group_signal_kill == OK_SIGKILL && native_group_signal_deferred_fatal == 1 &&
              native_group_signal_partial_success == 1 && native_group_signal_broadcast == 0 &&
              native_group_signal_stop_continue == 0 && native_group_signal_job_control == 0);
void copy_bytes(void *dst, const void *src, usize size)
{
    auto *out = static_cast<std::byte *>(dst);
    auto *in = static_cast<const std::byte *>(src);
    for (usize i = 0; i < size; ++i)
        out[i] = in[i];
}
alignas(4096) std::array<std::byte, memory::KernelEntryStackPool::arena_bytes> entry_stack_arena;
memory::KernelEntryStackPool entry_stacks;
SyscallOrigins syscall_origins{entry_stacks};
VfsStatContinuation<lu::configured_native_profile()> vfs_continuations{entry_stacks};
bool started{};
Kernel *runtime_kernel{};
Status runtime_failure{};
ShutdownOwner original_shutdown_owner{};
bool runtime_step_active{}, shutdown_admission_closed{}, shutdown_retirement_started{}, shutdown_retirement_completed{};
namespace
{
Status shutdown_failure{};
} // namespace
std::array<QuarantinedEntryStack, memory::KernelEntryStackPool::capacity> stranded_entry_stacks{};
Status cancel_native_vfs_thread(const sched::ThreadControlBlock &thread)
{
    const auto pending = vfs_continuations.pending(thread.owner, thread.tid);
    if (!pending.request) return {};
    if (!runtime_kernel)
        return Status::not_initialized("VFS retirement requires its scheduler service owner");
    if (runtime_kernel->scheduler().find_thread(thread.owner, thread.tid) != &thread)
        return Status::denied("VFS retirement requires its canonical native thread");
    return vfs_continuations.cancel(runtime_kernel->scheduler(), pending);
}
Status retire_native_thread(sched::ThreadControlBlock &thread
#ifdef OK_PRIVATE_POLL_MASK
                            , std::string_view signal_wait_cause
#endif
)
{
    if (linux_udp_receive_object(thread.owner, thread.tid).pool)
        return Status::busy("native thread retirement must first retire its Linux UDP receive owner");
    if (auto status = cancel_native_vfs_thread(thread); !status) return status;
#ifdef OK_PRIVATE_POLL_MASK
    const auto signal_lease = thread.entry_stack;
#endif
    if (auto status = syscall_origins.retire_checked(thread); !status) return status;
    if (auto status = vfs_continuations.retire(thread); !status) return status;
#ifdef OK_PRIVATE_POLL_MASK
    poll_signal_masks.restore(thread);
    if (const auto *wait = signal_waits.find(thread)) private_signal_wait_trace_retire(*wait, signal_wait_cause);
    signal_waits.restore(thread);
#endif
#ifdef OK_PRIVATE_MIPS_O32_SIGNAL
    private_mips_revoke(thread);
#endif
#ifdef OK_PRIVATE_MIPS64_N64_SIGNAL
    private_n64_revoke(thread);
#endif
#ifdef OK_PRIVATE_PPC32_SIGNAL
    private_ppc_revoke(thread);
#endif
    const auto retired = retire_entry_stack(entry_stacks, thread);
#ifdef OK_PRIVATE_POLL_MASK
    if (retired) private_signal_thread_retire(thread.owner, thread.tid, signal_lease);
#endif
    return retired;
}
Status prepare_native_thread(sched::ThreadControlBlock &thread)
{
    if (linux_udp_receive_object(thread.owner, thread.tid).pool)
        return Status::busy("native thread preparation must not replace a Linux UDP receive owner");
    auto status = prepare_entry_stack(entry_stacks, thread);
    if (status)
    {
        status = syscall_origins.prepare(thread);
        if (status)
        {
            status = vfs_continuations.prepare(thread);
            if (!status) syscall_origins.retire(thread);
        }
        if (!status)
            if (auto retired = retire_entry_stack(entry_stacks, thread); !retired) status = retired;
    }
#ifdef OK_PRIVATE_MIPS64_N64_SIGNAL
    if (thread.entry_stack.pool) private_n64_revoke(thread);
#endif
#ifdef OK_PRIVATE_PPC32_SIGNAL
    if (thread.entry_stack.pool) private_ppc_revoke(thread);
#endif
    if (!status && thread.entry_stack.pool)
    {
        // An unexpected post-reservation corruption must survive destruction of
        // an unpublished TCB. Keep the canonical charge and its recovery handle.
        stranded_entry_stacks[thread.entry_stack.slot] = {{thread.owner, thread.tid}, thread.entry_stack};
        thread.entry_stack = {};
        thread.kernel_stack_top = 0;
        runtime_failure = status;
        started = false;
    }
#ifdef OK_PRIVATE_MIPS_O32_SIGNAL
    if (status && !private_mips_reset(thread))
    {
        const auto retired = retire_native_thread(thread);
        if (!retired && thread.entry_stack.pool)
        {
            stranded_entry_stacks[thread.entry_stack.slot] = {{thread.owner, thread.tid}, thread.entry_stack};
            thread.entry_stack = {};
            thread.kernel_stack_top = 0;
            runtime_failure = retired;
            started = false;
        }
        return Status::fault("private MIPS signal lease preparation failed");
    }
#endif
    return status;
}
extern "C" void ok_platform_console_write_char(char) __attribute__((weak));
void emit(std::string_view text)
{
    if (ok_platform_console_write_char != nullptr)
        for (char c : text)
            ok_platform_console_write_char(c);
}
void emit_number(u64 value)
{
    char digits[20];
    usize count = 0;
    do { digits[count++] = static_cast<char>('0' + value % 10); value /= 10; } while (value);
    while (count) emit({&digits[--count], 1});
}
[[maybe_unused]] void emit_hex(u64 value)
{
    char digits[18] = {'0','x'};
    for (usize i=0;i<16;++i) digits[i+2] = "0123456789abcdef"[(value >> ((15-i)*4)) & 15];
    emit({digits,18});
}
void emit_entry_stack_stats()
{
    const auto stats = entry_stacks.accounting();
    if (!stats.backing_bytes) return; // Early scheduler diagnostics precede arena activation.
    static memory::KernelEntryStackAccounting previous{};
    static bool emitted{};
    if (emitted && stats.reservations == previous.reservations && stats.releases == previous.releases &&
        stats.activations == previous.activations && stats.quarantined_slots == previous.quarantined_slots &&
        stats.active_slots == previous.active_slots) return;
    previous = stats;
    emitted = true;
    emit("\nOK_ENTRY_STACKS version=1 scope=actual-user cpu=0 capacity=");
    emit_number(memory::KernelEntryStackPool::capacity);
    emit(" usable_bytes="); emit_number(memory::KernelEntryStackPool::usable_bytes);
    emit(" backing_bytes="); emit_number(stats.backing_bytes);
    emit(" leased="); emit_number(stats.leased_slots);
    emit(" active="); emit_number(stats.active_slots);
    emit(" quarantine="); emit_number(stats.quarantined_slots);
    emit(" peak="); emit_number(stats.peak_slots);
    emit(" reservations="); emit_number(stats.reservations);
    emit(" releases="); emit_number(stats.releases);
    emit(" activations="); emit_number(stats.activations);
    emit(" syscalls="); emit_number(entry_syscalls);
    emit(" timers="); emit_number(entry_timers);
    emit(" faults="); emit_number(entry_faults);
    emit(" rejected="); emit_number(entry_rejections);
    emit(" guards=mapped-canaries smp=0\n");
}
namespace
{
Status validate_group_stacks(const sched::ProcessControlBlock &process)
{
    for (const auto &thread : process.threads())
        if (auto status = validate_entry_stack(entry_stacks, thread); !status) return status;
    return Status::success();
}
Status retire_group_stacks(sched::ProcessControlBlock &process)
{
    if (auto status = validate_group_stacks(process); !status) return status;
    for (auto &thread : process.threads())
        if (auto status = retire_native_thread(thread
#ifdef OK_PRIVATE_POLL_MASK
                                               , "group"
#endif
            ); !status) return status;
    return Status::success();
}
} // namespace
Status string_from_user(const hw::AddressSpace &space, uptr address, FixedString<256> &result)
{
    result.clear();
    for (usize i = 0; i < 256; ++i)
    {
        char c;
        if (address + i < address || !hw::copy_from_user(space, &c, address + i, 1))
            return Status::fault("invalid user string");
        if (c == 0)
            return Status::success();
        if (auto status = result.append(c); !status.ok())
            return status;
    }
    return Status::overflow("user string exceeds limit");
}
i64 error(Status status)
{
    return -syscall::ErrnoMapper::errno_for(status);
}
Status encode_result(const sched::ProcessControlBlock &process, hw::UserFrame &frame, i64 result)
{
    if (process.resources().personality == Personality::linux)
    {
        if (auto status = linux_bootstrap::complete<lu::configured_native_profile()>(frame, result); !status)
            return status;
    }
    else hw::set_return_value(frame, result);
    return Status::success();
}
Status set_result(const sched::ProcessControlBlock &process, hw::UserFrame &frame, i64 result)
{
    syscall_origins.revoke_frame(process, frame);
    if (auto status = encode_result(process, frame, result); !status) return status;
#ifdef OK_PRIVATE_MIPS64_N64_SIGNAL
    return private_n64_completion(process, frame, result);
#elif defined(OK_PRIVATE_PPC32_SIGNAL)
    return private_ppc_completion(process, frame, result);
#else
    return Status::success();
#endif
}

void finish(Kernel &kernel, sched::ProcessId pid, i32 code, u8 signal)
{
    const auto *initial = kernel.scheduler().find(pid);
    if (!initial) return;
    if (initial->state() == sched::ProcessState::zombie || initial->state() == sched::ProcessState::exited)
    { runtime_failure = Status::already_exists("native group already completed"); started = false; return; }
#ifdef OK_PRIVATE_POLL_MASK
    PrivateChildExit::Completion completion{};
    PrivateChildExit::Adoption adoption{};
    const auto cause = signal ? sched::ChildExitStatus::signaled(signal) : sched::ChildExitStatus::normal(code);
    if (auto status = PrivateChildExit::prepare(kernel.scheduler(), pid, cause, private_signal_find,
        [](const sched::ThreadControlBlock &thread) { return signal_waits.real_blocked(thread); }, completion); !status)
    { runtime_failure = status; started = false; return; }
    if (auto status = PrivateChildExit::prepare_adoption(kernel.scheduler(), pid, private_signal_find, adoption); !status)
    { runtime_failure = status; started = false; return; }
#endif
    auto cleanup = [&]() -> Status {
        cancel_poll(kernel, pid);
        if (auto status = cancel_futex(kernel, pid); !status) return status;
        cancel_pipe_read(kernel, pid);
        cancel_pipe_write(kernel, pid);
        cancel_pty_waits(kernel, pid);
        cancel_child_wait(kernel, pid);
        retire_native_udp_receives(kernel, pid);
        const auto linux_udp_retired = retire_linux_udp_receives(kernel, pid, {}, LinuxUdpReceiveRetirement::process_exit);
        if (!runtime_failure) return runtime_failure;
        if (!linux_udp_retired) return linux_udp_retired;
        auto *process = kernel.scheduler().find(pid);
        if (!process) return Status::fault("native completion lost process");
        if (auto status = retire_group_stacks(*process); !status) return status;
        if (!runtime_failure) return runtime_failure;
#ifdef OK_PRIVATE_POLL_MASK
        private_signal_retire(pid);
#endif
        clear_group_tids(kernel, *process);
        if (!runtime_failure) return runtime_failure;
        release(process->resources().native_space, process->resources().native_pages);
        if (!runtime_failure) return runtime_failure;
        emit_pool_stats();
        emit_entry_stack_stats();
        for (auto &thread : process->threads())
        {
            thread.native_deadline.cancel();
            thread.native_active = false;
        }
        return Status::success();
    };
    auto transition = [&]() {
        return signal ? kernel.scheduler().terminate_process(pid, signal) : kernel.posix().exit(pid, code);
    };
#ifdef OK_PRIVATE_POLL_MASK
    const auto status = child_exit_transaction(cleanup, transition,
        [&] {
            if (auto status = private_child_exit_commit(kernel, completion); !status) return status;
            return PrivateChildExit::commit_adoption(kernel.scheduler(), adoption, private_signal_find);
        });
#else
    auto status = cleanup();
    if (status) status = transition();
#endif
    if (!status) { runtime_failure = status; started = false; }
}
} // namespace runtime_detail
namespace
{
Status hold_shutdown_failure(Status status)
{
    if (!status && shutdown_failure) shutdown_failure = status;
    return shutdown_failure;
}
Status shutdown_runtime_owner(Kernel &kernel)
{
    if (runtime_kernel != &kernel || !original_shutdown_owner.runtime)
        return Status::not_initialized("native shutdown has no original runtime owner");
    if (!shutdown_failure) return shutdown_failure;
    if (!runtime_failure) return runtime_failure;
    return {};
}
Status original_init_state(Kernel &kernel)
{
    const auto *process = kernel.scheduler().find(original_shutdown_owner.process);
    const auto *leader = kernel.scheduler().find_thread(original_shutdown_owner.process, original_shutdown_owner.leader);
    if (!process || !leader || process->pid() != 1 || process->parent_pid() ||
        process->execution() != sched::ProcessExecution::user_process ||
        process->resources().personality != Personality::native ||
        process->state() == sched::ProcessState::created || process->state() == sched::ProcessState::zombie ||
        process->state() == sched::ProcessState::exited || leader->state == sched::ProcessState::exited ||
        leader->owner != process->pid() || !leader->native_active || !process->resources().native_space.root ||
        process->resources().native_pages.empty())
        return Status::denied("shutdown original init is not a live native process");
    const auto &ids = process->credentials();
    if (ids.kernel_space || ids.uid || ids.euid || ids.suid || ids.gid || ids.egid || ids.sgid)
        return Status::denied("shutdown original init requires all six root identities");
    usize live{};
    for (const auto &thread : process->threads())
        live += thread.state != sched::ProcessState::exited;
    if (live != 1 || !leader->entry_stack.pool || !leader->kernel_stack_top)
        return Status::denied("shutdown original init must retain its single leader");
    return validate_entry_stack(entry_stacks, *leader);
}
Status shutdown_quiesced_groups(Kernel &kernel, ShutdownOwner owner)
{
    const auto *init = kernel.scheduler().find(owner.process);
    if (!init || !init->children().empty())
        return Status::busy("native shutdown requires original init to reap every child");
    for (const auto &process : kernel.scheduler().processes())
    {
        if (process.execution() != sched::ProcessExecution::user_process || process.pid() == owner.process) continue;
        if (process.parent_pid() == owner.process)
            return Status::busy("native shutdown retains an unresolved original-init child");
        if (process.state() != sched::ProcessState::zombie && process.state() != sched::ProcessState::exited)
            return Status::busy("native shutdown requires quiesced user child groups");
        if (process.resources().native_space.root || !process.resources().native_pages.empty())
            return Status::busy("completed user child still retains native VM ownership");
        for (const auto &thread : process.threads())
            if (thread.native_active || thread.entry_stack.pool || thread.kernel_stack_top || thread.syscall_pending ||
                thread.wait_registration.active() || thread.native_deadline.armed())
                return Status::busy("completed user child still retains native continuation ownership");
    }
    return {};
}
Status native_retirement_empty(const NativeShutdownReport &report)
{
    if (!report.failure) return report.failure;
    if (report.step_active || report.live_user_groups || report.native_threads || report.address_spaces ||
        report.mapping_pages || report.backend_spaces || report.active_entries || report.leased_entries ||
        report.quarantined_entries || report.stranded_entries || report.quarantined_spaces ||
        report.quarantined_pins || report.wait_members || report.signal_owners || report.signal_waits ||
        report.origin_records || report.origin_borrows || report.origin_pending || report.vfs_records ||
        report.vfs_pending || report.vfs_captures || report.live_objects || report.object_pages || report.object_quarantine)
        return Status::busy("native shutdown retains an execution, continuation or VM owner");
    return {};
}
} // namespace
Result<ShutdownOwner> shutdown_owner(Kernel &kernel)
{
    if (runtime_kernel != &kernel || !original_shutdown_owner.runtime)
        return Status::not_initialized("native runtime has not bound original init");
    return original_shutdown_owner;
}
Result<ShutdownCaller> shutdown_caller(Kernel &kernel)
{
    if (auto status = shutdown_runtime_owner(kernel); !status) return status;
    if (!started || shutdown_admission_closed)
        return Status::busy("native shutdown caller admission is closed");
    if (auto status = original_init_state(kernel); !status) return status;
    const auto *thread = kernel.scheduler().current_thread();
    const auto *process = kernel.scheduler().find(kernel.scheduler().current_pid());
    if (!process || !thread || process->pid() != original_shutdown_owner.process ||
        thread != kernel.scheduler().find_thread(process->pid(), original_shutdown_owner.leader) ||
        thread->state != sched::ProcessState::running || process->state() != sched::ProcessState::running)
        return Status::denied("shutdown caller is not the canonical running original init leader");
    const auto &ids = process->credentials();
    return ShutdownCaller{original_shutdown_owner, thread->tid, 1, true,
                          {ids.uid, ids.euid, ids.suid, ids.gid, ids.egid, ids.sgid}};
}
Status shutdown_preflight(Kernel &kernel, ShutdownOwner owner)
{
    if (auto status = shutdown_runtime_owner(kernel); !status) return status;
    if (owner != original_shutdown_owner)
        return Status::denied("native shutdown owner is foreign or stale");
    if (!started || shutdown_admission_closed)
        return Status::busy("native shutdown runtime is not accepting a reservation");
    if (auto status = original_init_state(kernel); !status) return status;
    if (entry_stacks.accounting().active_slots)
        return Status::busy("native shutdown reservation has an active entry");
    // Service children remain admissible until the OS has quiesced them.
    const auto report = shutdown_report(kernel);
    if (!report.failure) return report.failure;
    if (report.backend_spaces != report.address_spaces)
        return Status::fault("native shutdown found an unpublished backend address-space owner");
    if (report.quarantined_entries || report.stranded_entries || report.quarantined_spaces ||
        report.quarantined_pins || report.object_quarantine || report.quarantined_pages)
        return Status::busy("native shutdown has unresolved quarantined ownership");
    if (report.origin_borrows || report.origin_pending)
        return Status::busy("native shutdown has borrowed or deferred origin retirement");
    return {};
}
Status shutdown_check_commit(Kernel &kernel, ShutdownOwner owner)
{
    if (auto status = shutdown_preflight(kernel, owner); !status) return status;
    if (auto status = shutdown_quiesced_groups(kernel, owner); !status) return status;
    if (auto status = kernel.scheduler().shutdown_console_status(); !status) return status;
    if (auto status = kernel.scheduler().shutdown_endpoint_status(); !status) return status;
    return kernel.vfs().socket_path_reference_count()
        ? Status::busy("native shutdown retains a socket path reference") : Status{};
}
Status shutdown_outer_boundary(Kernel &kernel)
{
    if (auto status = shutdown_runtime_owner(kernel); !status) return status;
#ifdef OK_KERNEL_FREESTANDING
    if (auto status = arch::require_service_stack(); !status) return status;
#endif
    if (runtime_step_active || entry_stacks.accounting().active_slots)
        return Status::busy("native shutdown requires return from the complete runtime step");
    const auto backend = hw::ownership_report();
    if (!backend.failure) return backend.failure;
    return backend.executing ? Status::busy("native backend still owns an active execution frame") : Status{};
}
Status shutdown_close_admission(Kernel &kernel)
{
    if (auto status = shutdown_outer_boundary(kernel); !status) return status;
    if (shutdown_admission_closed) return Status::already_exists("native shutdown admission was already consumed");
    shutdown_admission_closed = true;
    return {};
}
NativeShutdownReport shutdown_report(Kernel &kernel)
{
    NativeShutdownReport result{};
    auto retain_failure = [&](Status status) { if (result.failure && !status) result.failure = status; };
    retain_failure(shutdown_runtime_owner(kernel));
    result.admission_closed = shutdown_admission_closed;
    result.step_active = runtime_step_active;
    result.retirement_started = shutdown_retirement_started;
    result.retirement_completed = shutdown_retirement_completed;
    for (const auto &process : kernel.scheduler().processes())
    {
        if (process.execution() != sched::ProcessExecution::user_process) continue;
        result.live_user_groups += process.state() != sched::ProcessState::zombie && process.state() != sched::ProcessState::exited;
        result.address_spaces += process.resources().native_space.root != 0;
        result.mapping_pages += process.resources().native_pages.size();
        for (const auto &thread : process.threads())
        {
            result.native_threads += thread.native_active;
            result.wait_members += thread.wait_registration.active() || thread.native_deadline.armed() || thread.syscall_pending;
        }
    }
    const auto backend = hw::ownership_report();
    retain_failure(backend.failure);
    result.backend_spaces = backend.address_spaces;
    if (backend.executing) retain_failure(Status::busy("native backend still executing"));
    const auto entries = entry_stacks.accounting();
    result.active_entries = entries.active_slots;
    result.leased_entries = entries.leased_slots;
    result.quarantined_entries = entries.quarantined_slots;
    for (const auto &entry : stranded_entry_stacks) result.stranded_entries += entry.handle.pool != nullptr;
    for (const auto &space : quarantined_spaces)
    {
        result.quarantined_spaces += space.used || space.reserved || space.space.root || !space.pages.empty();
        result.quarantined_pins += space.pinned_count;
    }
    auto members = [&](const auto &waits) {
        for (const auto &entry : waits) result.wait_members += entry.pid || entry.tid || entry.object.pool;
    };
    members(poll_waits); members(futex_waits); members(pipe_read_waits); members(pipe_write_waits);
    members(pty_waits);
    members(child_waits); members(native_udp_receive_waits); members(linux_udp_receive_waits);
    for (const auto &entry : linux_udp_receive_waits) result.wait_members += entry.reservation != 0;
    retain_failure(linux_udp_receive_store.failure());
    if (linux_udp_receive_store.contained()) retain_failure(Status::fault("Linux UDP owner store is contained"));
    auto objects = [&](const auto &pool) {
        retain_failure(pool.ownership_status());
        const auto info = pool.accounting();
        result.live_objects += info.live_objects;
        result.object_pages += info.backing_pages;
        result.object_quarantine += info.quarantined_pages;
    };
    objects(poll_objects); objects(futex_objects); objects(pipe_read_objects); objects(pipe_write_objects);
    objects(pty_wait_objects);
    objects(child_wait_objects); objects(native_udp_receive_objects); objects(linux_udp_receive_objects);
    const auto origins = syscall_origins.ownership();
    result.origin_records = origins.prepared + origins.live + origins.bound;
    result.origin_borrows = origins.borrows;
    result.origin_pending = origins.pending;
    const auto continuations = vfs_continuations.ownership();
    result.vfs_records = continuations.prepared;
    result.vfs_pending = continuations.pending;
    result.vfs_captures = continuations.captured;
#ifdef OK_PRIVATE_POLL_MASK
    for (const auto &entry : private_signal_actions)
    {
        result.signal_owners += entry.pid || entry.shared.identity().process || entry.group_kill || entry.native_termination;
        for (auto word : entry.shared.pending_mask().words) result.signal_owners += word != 0;
    }
    for (usize i = 0; i < private_signal_queues.size(); ++i)
    {
        const auto &queue = private_signal_queues[i];
        result.signal_owners += queue.identity().process != 0;
        for (auto word : queue.pending_mask().words) result.signal_owners += word != 0;
        const auto &stack = private_signal_altstacks[i];
        result.signal_owners += stack.base || stack.size || stack.flags != sched::SignalAltstack::disable;
    }
    for (const auto &entry : poll_signal_masks.records()) result.signal_waits += entry.phase != PollSignalMasks::Phase::none;
    for (const auto &entry : signal_waits.records()) result.signal_waits += entry.phase != decltype(signal_waits)::Phase::none;
#endif
#ifdef OK_PRIVATE_MIPS_O32_SIGNAL
    for (const auto &entry : private_mips_states) result.signal_owners += entry.state() != PrivateMipsReadiness::State::unverified;
#endif
#ifdef OK_PRIVATE_MIPS64_N64_SIGNAL
    for (const auto &entry : private_n64_states) result.signal_owners += entry.readiness.state() != PrivateN64Readiness::State::unverified;
#endif
#ifdef OK_PRIVATE_PPC32_SIGNAL
    for (const auto &entry : private_ppc_states)
        result.signal_owners += entry.initial || entry.initial_relocation || entry.initial_space || entry.staged_space.root ||
                                entry.readiness.state() != PrivatePpcReadiness::State::unverified ||
                                entry.readiness.transaction() != PrivatePpcReadiness::Transaction::none;
#endif
    result.pool_pages = page_pool.used_count();
    result.user_pages = page_pool.user_count();
    result.pool_pins = page_pool.pinned_count();
    result.pending_pages = page_pool.pending_count();
    result.quarantined_pages = page_pool.quarantined_kernel_page_count();
    const auto files = shared_files.ledger();
    retain_failure(files.first_cleanup_failure);
    result.shared_files = files.files;
    result.shared_aliases = files.aliases;
    result.shared_retiring = files.retiring_files;
    result.shared_frames = files.retained_frames;
    result.shared_reservations = files.reserved_cells;
    const auto resources = kernel.scheduler().resource_drain_report();
    retain_failure(resources.first_failure);
    if (resources.active) retain_failure(Status::busy("scheduler resource drain remains active"));
    return result;
}
Status shutdown_retire(Kernel &kernel)
{
    if (auto status = shutdown_outer_boundary(kernel); !status) return hold_shutdown_failure(status);
    if (!shutdown_admission_closed) return Status::denied("native retirement requires closed admission");
    if (shutdown_retirement_started) return Status::already_exists("native shutdown retirement cannot be retried");
    if (auto status = original_init_state(kernel); !status) return hold_shutdown_failure(status);
    if (auto status = shutdown_quiesced_groups(kernel, original_shutdown_owner); !status) return hold_shutdown_failure(status);
    shutdown_retirement_started = true;
    finish(kernel, original_shutdown_owner.process, 0, 0);
    if (!runtime_failure) return hold_shutdown_failure(runtime_failure);
    auto trim = [&](auto &pool) { return pool.trim_empty_pages(); };
    if (auto status = trim(poll_objects); !status) return hold_shutdown_failure(status);
    if (auto status = trim(futex_objects); !status) return hold_shutdown_failure(status);
    if (auto status = trim(pipe_read_objects); !status) return hold_shutdown_failure(status);
    if (auto status = trim(pipe_write_objects); !status) return hold_shutdown_failure(status);
    if (auto status = trim(pty_wait_objects); !status) return hold_shutdown_failure(status);
    if (auto status = trim(child_wait_objects); !status) return hold_shutdown_failure(status);
    if (auto status = trim(native_udp_receive_objects); !status) return hold_shutdown_failure(status);
    if (auto status = trim(linux_udp_receive_objects); !status) return hold_shutdown_failure(status);
    started = false;
    if (auto status = native_retirement_empty(shutdown_report(kernel)); !status) return hold_shutdown_failure(status);
    shutdown_retirement_completed = true;
    return {};
}
Status shutdown_verify_drained(Kernel &kernel)
{
    if (auto status = shutdown_outer_boundary(kernel); !status) return hold_shutdown_failure(status);
    if (!shutdown_admission_closed || !shutdown_retirement_completed)
        return Status::denied("native shutdown retirement has not completed");
    if (auto status = kernel.scheduler().shutdown_console_status(); !status) return hold_shutdown_failure(status);
    if (auto status = kernel.scheduler().shutdown_endpoint_status(); !status) return hold_shutdown_failure(status);
    if (kernel.vfs().socket_path_reference_count())
        return hold_shutdown_failure(Status::busy("native shutdown retains a socket path reference"));
    const auto report = shutdown_report(kernel);
    if (auto status = native_retirement_empty(report); !status) return hold_shutdown_failure(status);
    if (report.pool_pages || report.user_pages || report.pool_pins || report.pending_pages || report.quarantined_pages ||
        report.shared_files || report.shared_aliases || report.shared_retiring || report.shared_frames || report.shared_reservations)
        return hold_shutdown_failure(Status::busy("native shutdown retains page or shared-file ownership"));
    return {};
}

Status runtime_status()
{
    return shutdown_failure ? runtime_failure : shutdown_failure;
}
Status terminate_native_process(Kernel &kernel, u64 pid, u8 signal)
{
    const auto *process = kernel.scheduler().find(pid);
    if (!pid || !process) return Status::not_found("native termination process not found");
    if (process->execution() != sched::ProcessExecution::user_process || process->state() == sched::ProcessState::created)
        return Status::invalid_argument("native termination requires a published user process");
    if (process->state() == sched::ProcessState::zombie || process->state() == sched::ProcessState::exited)
        return Status::already_exists("native process has already completed");
    if (!signal || signal > SignalActionCodec<lu::configured_native_profile()>::max_signal)
        return Status::invalid_argument("invalid native termination signal");
    if (runtime_kernel != &kernel)
        return Status::not_initialized("native termination requires its service owner");
    if (!runtime_failure) return runtime_failure;
    if (!started) return Status::not_initialized("native termination requires a running service");
    if (entry_stacks.accounting().active_slots)
        return Status::busy("native termination requires an inactive entry boundary");
    if (!process->resources().native_space.root || process->resources().native_pages.empty())
        return Status::invalid_argument("native termination requires a retained address space");
    usize live{};
    for (const auto &thread : process->threads())
        if (thread.state != sched::ProcessState::exited)
        {
            if (!thread.native_active || !thread.entry_stack.pool || !thread.kernel_stack_top)
                return Status::invalid_argument("native termination requires live native threads and entry leases");
            if (auto status = validate_entry_stack(entry_stacks, thread); !status) return status;
            ++live;
        }
    if (!live) return Status::invalid_argument("native termination has no live native thread");
    finish(kernel, pid, 0, signal);
    return runtime_failure;
}
void release_process(Kernel &kernel, u64 pid)
{
    cancel_poll(kernel, pid);
    if (!cancel_futex(kernel, pid)) return;
    cancel_pipe_read(kernel, pid);
    cancel_pipe_write(kernel, pid);
    cancel_pty_waits(kernel, pid);
    cancel_child_wait(kernel, pid);
    retire_native_udp_receives(kernel, pid);
    const auto linux_udp_retired = retire_linux_udp_receives(kernel, pid, {}, LinuxUdpReceiveRetirement::process_exit);
    if (!runtime_failure) return;
    if (!linux_udp_retired) { runtime_failure = linux_udp_retired; started = false; return; }
    if (auto *process = kernel.scheduler().find(pid))
    {
        if (auto status = retire_group_stacks(*process); !status)
        { runtime_failure = status; return; }
#ifdef OK_PRIVATE_POLL_MASK
        private_signal_retire(pid);
#endif
        clear_group_tids(kernel, *process);
        release(process->resources().native_space, process->resources().native_pages);
        emit_entry_stack_stats();
        for (auto &thread : process->threads())
        {
            thread.native_deadline.cancel();
            thread.wait_registration.cancel();
            thread.native_active = false;
        }
    }
}
void input(char value)
{
    if (!shutdown_admission_closed && !native_shutdown_pending() && runtime_kernel != nullptr)
        runtime_kernel->scheduler().console_input(value);
}
} // namespace ok::user::native
