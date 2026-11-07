// PTY pair I/O continuations and their owners (raw pairs and ptmx-style pairs),
// for both personalities. A Linux wait binds its syscall origin when armed; the
// terminal signal family (jobctl.cpp) interrupts or restarts it. A read that
// waits below VMIN or with VMIN 0 runs its VTIME timer (expire_tty_timers).
// Slave reads, and slave writes under TOSTOP, pass the job-control gate at
// entry and again when a continue ended their wait.
#include "private.hpp"

namespace ok::user::native
{
inline namespace runtime_detail
{
// Private serialized runtime owners. Endpoint generations are never packed into wait keys.
static_assert(sched::private_pty::RawPtyTable<>::queue_capacity == sched::pipe_capacity,
              "PTY staging uses the separately checked native vector leaf capacity");
constinit PtyWaitObjects pty_wait_objects{page_pool, pty_wait_capacity};
std::array<PollWaitIndex, pty_wait_capacity> pty_waits{};
memory::ObjectHandle pty_wait_object(sched::ProcessId pid, sched::ThreadId tid)
{
    for (const auto &entry : pty_waits)
        if (entry.pid == pid && entry.tid == tid) return entry.object;
    return {};
}
SyscallOrigins::PtyReadBinding pty_read_origin_binding(memory::ObjectHandle object, const PtyWait &wait)
{
    return {object, wait.ticket, wait.request, wait.fd, wait.address, wait.count, wait.description, wait.endpoint, wait.vectors};
}
SyscallOrigins::PtyWriteBinding pty_write_origin_binding(memory::ObjectHandle object, const PtyWait &wait)
{
    return {object, wait.ticket, wait.request, wait.fd, wait.address, wait.count, wait.description, wait.endpoint,
            wait.vectors, wait.committed};
}
namespace
{
u64 pty_wait_key(sched::private_pty::Handle endpoint)
{ return (static_cast<u64>(endpoint.slot) << 1) | (endpoint.side == sched::private_pty::Side::slave ? 1u : 0u); }
sched::ThreadWaitReason pty_wait_reason(bool writing)
{ return writing ? sched::ThreadWaitReason::pty_write : sched::ThreadWaitReason::pty_read; }
void emit_pty_wait(const PtyWait &wait, bool terminal, std::string_view stage, i64 result = 0)
{
#ifdef OK_ENABLE_TEST_POINTS
    emit(terminal ? "\nOK_NATIVE_PTY_COMPLETE pid=" : "\nOK_NATIVE_PTY_WAIT pid="); emit_number(wait.pid);
    emit(" tid="); emit_number(wait.tid); emit(" wait="); emit_number(wait.ticket.generation);
    emit(" request="); emit_number(wait.request); emit(" slot="); emit_number(wait.endpoint.slot);
    emit(" generation="); emit_number(wait.endpoint.generation);
    emit(wait.endpoint.side == sched::private_pty::Side::master ? " side=master" : " side=slave");
    emit(wait.writing ? " operation=write" : " operation=read");
    emit(" description="); emit_number(wait.description); emit(" committed="); emit_number(wait.committed);
    emit(" stage="); emit(stage);
    if (terminal) { emit(" result="); if (result < 0) { emit("-"); emit_number(u64{0} - static_cast<u64>(result)); }
        else emit_number(static_cast<u64>(result)); }
    emit(" live="); emit_number(pty_wait_objects.accounting().live_objects); emit("\n");
#else
    static_cast<void>(wait); static_cast<void>(terminal); static_cast<void>(stage); static_cast<void>(result);
#endif
}
// The bound origin of a Linux wait goes with it.
void pty_retire_origin(memory::ObjectHandle object, const PtyWait &wait)
{
    if (!wait.origin) return;
    if (wait.writing) syscall_origins.retire_terminal(wait.pid, wait.tid, pty_write_origin_binding(object, wait));
    else syscall_origins.retire_terminal(wait.pid, wait.tid, pty_read_origin_binding(object, wait));
}
} // namespace
void cancel_pty_wait(Kernel &kernel, memory::ObjectHandle object, std::string_view outcome, i64 result)
{
    auto resolved = pty_wait_objects.resolve(object);
    if (!resolved) { runtime_failure = resolved.status(); return; }
    const auto wait = *resolved.value();
    pty_retire_origin(object, wait);
    for (auto &entry : pty_waits) if (entry.object == object) entry = {};
    if (wait.ticket.generation)
        static_cast<void>(kernel.scheduler().complete_wait(wait.ticket, sched::WaitCompletion::cancelled));
    if (wait.description != sched::invalid_description)
        if (auto status = kernel.scheduler().release_description(wait.description); !status) runtime_failure = status;
    if (auto status = pty_wait_objects.destroy(object); !status) runtime_failure = status;
    if (auto status = pty_wait_objects.trim_empty_pages(); !status) runtime_failure = status;
    if (wait.ticket.generation) emit_pty_wait(wait, true, outcome, result);
}
void cancel_pty_waits(Kernel &kernel, sched::ProcessId pid, sched::ThreadId tid)
{
    for (const auto &entry : pty_waits)
        if (entry.pid == pid && (!tid || entry.tid == tid)) cancel_pty_wait(kernel, entry.object);
}
namespace
{
bool pty_owner_current(const sched::ProcessControlBlock &process, const sched::ThreadControlBlock &thread,
                       const PtyWait &wait)
{
    const auto &space = process.resources().native_space;
    return process.pid() == wait.pid && thread.owner == wait.pid && thread.tid == wait.tid &&
        process.resources().personality == wait.personality && thread.native_active && thread.syscall_pending &&
        wait.lease.pool && thread.entry_stack == wait.lease && validate_entry_stack(entry_stacks, thread) &&
        SyscallOrigins::same_frame(thread.native_frame, wait.frame) &&
        space.root == wait.space.root && space.slot == wait.space.slot && space.generation == wait.space.generation;
}
} // namespace
void prune_pty_waits(Kernel &kernel)
{
    for (const auto &entry : pty_waits)
    {
        if (!entry.pid) continue;
        const auto object = entry.object;
        auto resolved = pty_wait_objects.resolve(object);
        if (!resolved) { runtime_failure = resolved.status(); continue; }
        const auto &wait = *resolved.value();
        const auto *process = kernel.scheduler().find(wait.pid);
        const auto *thread = kernel.scheduler().find_thread(wait.pid, wait.tid);
        if (!process || !thread || !pty_owner_current(*process, *thread, wait) ||
            process->state() == sched::ProcessState::zombie || process->state() == sched::ProcessState::exited ||
            thread->state == sched::ProcessState::exited || thread->wait_registration.ticket() != wait.ticket ||
            (thread->wait_registration.active() &&
             (thread->state != sched::ProcessState::blocked || thread->wait_reason != pty_wait_reason(wait.writing) ||
              thread->wait_key != pty_wait_key(wait.endpoint)))) cancel_pty_wait(kernel, object);
    }
}
namespace
{
i64 pty_terminal_result(const PtyWait &wait, i64 result)
{ return wait.writing && wait.committed ? static_cast<i64>(wait.committed) : result; }
Status gather_pty_suffix(const hw::AddressSpace &space, std::span<std::byte> bytes, const PtyWait &wait)
{
    if (!wait.vectors.count)
    {
        if (wait.address > ~uptr{0} - wait.committed) return Status::fault("PTY input range wraps");
        return hw::copy_from_user(space, bytes.data(), wait.address + wait.committed, bytes.size());
    }
    usize skip = wait.committed, copied = 0;
    for (usize index = 0; index < wait.vectors.count && copied < bytes.size(); ++index)
    {
        const auto &vector = wait.vectors.entries[index];
        if (skip >= vector.length) { skip -= vector.length; continue; }
        const auto length = vector.length - skip < bytes.size() - copied ? vector.length - skip : bytes.size() - copied;
        if (vector.base > ~uptr{0} - skip) return Status::fault("PTY vector range wraps");
        if (auto status = hw::copy_from_user(space, bytes.data() + copied, vector.base + skip, length); !status) return status;
        copied += length; skip = 0;
    }
    return copied == bytes.size() ? Status{} : Status::fault("PTY input vector is short");
}
// `timer_expired`: a read retried after its VTIME timer ran out.
Result<usize> attempt_pty_io(Kernel &kernel, sched::ProcessControlBlock &process, const PtyWait &wait,
                             bool timer_expired = false)
{
    const auto *description = kernel.scheduler().open_description(wait.description);
    if (!description || !description->references || description->pty_endpoint != wait.endpoint ||
        !(wait.writing ? description->writable : description->readable)) return Status::bad_descriptor("PTY retained provider changed");
    auto &space = process.resources().native_space;
    if (!wait.writing)
    {
        // Immediate calls need the same OFD lifetime as retained continuations.
        if (auto status = kernel.scheduler().retain_description(wait.description); !status) return status;
        auto read = [&]() -> Result<usize>
        {
            if (!pipe_ranges(space, wait.address, wait.count, wait.vectors, true))
                return Status::fault("PTY output mapping changed");
            auto prepared = kernel.scheduler().prepare_pty_read(wait.description, wait.count, timer_expired, wait.vtime);
            if (!prepared) return prepared.status();
            auto &preview = prepared.value();
            if (preview.endpoint() != wait.endpoint)
            { preview.cancel(); return Status::bad_descriptor("PTY preview endpoint changed"); }
            // The plan is copied out first and consumed only after the copy worked.
            std::array<std::byte, sched::private_pty::RawPtyTable<>::queue_capacity> staged{};
            const auto count = preview.size();
            if (preview.copy({staged.data(), count}) != count)
            { preview.cancel(); return Status::busy("PTY preview changed before copy-out"); }
            if (!pipe_scatter(space, wait.address, {staged.data(), count}, wait.vectors))
            { preview.cancel(); return Status::fault("PTY output copy failed before dequeue"); }
            auto consumed = kernel.scheduler().commit_pty_read(preview);
            if (!consumed || consumed.value() != count)
            {
                preview.cancel();
                runtime_failure = Status::fault("PTY serialized read commit changed");
                return runtime_failure;
            }
            return count;
        }();
        if (auto status = kernel.scheduler().release_description(wait.description); !status)
        { runtime_failure = status; return status; }
        return read;
    }
    std::array<std::byte, sched::private_pty::RawPtyTable<>::queue_capacity> buffer{};
    auto readiness = kernel.scheduler().pty_description_readiness(wait.description);
    if (!readiness) return readiness.status();
    if (readiness.value().write_error) return Status::io_error("PTY peer retired");
    if (!readiness.value().writable) return Status::would_block("PTY outgoing queue is full");
    usize count = wait.count - wait.committed;
    if (count > buffer.size()) count = buffer.size();
    // The slave's N_TTY decides how much master input it takes (editing
    // bytes need no room); slave output is bounded by the master queue.
    if (wait.endpoint.side == sched::private_pty::Side::slave && count > readiness.value().outgoing_available)
        count = readiness.value().outgoing_available;
    if (auto status = gather_pty_suffix(space, {buffer.data(), count}, wait); !status) return status;
    return kernel.scheduler().write_pty_description(wait.description, {buffer.data(), count});
}
// A write goes on while it makes progress, sending ISIG signals after each
// pass. A master write stopped by a full signal outbox retries once after the
// drain freed it: the drain ran before any wait registered, so nothing would
// wake a parked writer later. Returns the bytes of this call or the last error.
Result<usize> attempt_pty_write(Kernel &kernel, sched::ProcessControlBlock &process, PtyWait &wait)
{
    const usize start = wait.committed;
    bool retry = true;
    while (true)
    {
        auto transferred = attempt_pty_io(kernel, process, wait);
        const bool drained = tty_drain_outbox(kernel);
        if (!transferred)
        {
            if (retry && drained && transferred.status().code() == StatusCode::would_block) { retry = false; continue; }
            return transferred;
        }
        wait.committed += transferred.value();
        retry = true;
        if (!transferred.value() || wait.committed == wait.count) return wait.committed - start;
    }
}
using PtyReadWait = decltype(sched::SchedulerState::PtyReadState{}.wait);
sched::TerminalRef pty_terminal_ref(sched::private_pty::Handle endpoint)
{ return {static_cast<u16>(endpoint.slot + 1), endpoint.generation}; }
// Arms, keeps or clears the VTIME timer of a read about to wait (Linux
// n_tty_read): VTIME is taken when the read first waits; with VMIN 0 one timer
// runs from the start of the read; below VMIN it restarts whenever a byte was
// received since it was armed.
Status pty_arm_timer(Kernel &kernel, PtyWait &wait)
{
    if (wait.writing) return {};
    auto state = kernel.scheduler().pty_read_state(wait.description, wait.count, wait.vtime);
    if (!state) return state.status();
    const auto &value = state.value();
    if (!wait.vtime.captured) wait.vtime = {value.vtime, true};
    if (!value.timer_deciseconds) { wait.deadline = 0; wait.timer = 0; wait.interbyte = false; return {}; }
    const bool interbyte = value.wait == PtyReadWait::interbyte;
    if (wait.deadline && wait.timer == value.timer_deciseconds && wait.interbyte == interbyte &&
        (!interbyte || value.received == wait.received_mark)) return {};
    const auto now = arch::monotonic_nanoseconds();
    if (!now) return now.status();
    wait.deadline = now.value() + u64{value.timer_deciseconds} * 100'000'000ULL;
    if (!wait.deadline) wait.deadline = 1;
    wait.timer = value.timer_deciseconds; wait.interbyte = interbyte; wait.received_mark = value.received;
    return {};
}
// A Linux wait carries its bound origin to the new ticket (and write progress).
void pty_rebind(sched::ProcessControlBlock &process, const sched::ThreadControlBlock &thread,
                memory::ObjectHandle object, const PtyWait &before, const PtyWait &after)
{
    if (!after.origin) return;
    const auto origin = syscall_origins.identity(process, thread);
    const bool kept = origin.capture == after.origin && (after.writing
        ? syscall_origins.rearm(origin, thread.native_frame, pty_write_origin_binding(object, before), pty_write_origin_binding(object, after))
        : syscall_origins.rearm(origin, thread.native_frame, pty_read_origin_binding(object, before), pty_read_origin_binding(object, after)));
    if (!kept) syscall_origins.revoke(thread);
}
} // namespace
// The next unwritten byte decides (an OPOST expansion or an ISIG character
// may not fit); an unreadable buffer is ready, its re-dispatch faults.
bool pty_write_ready(Kernel &kernel, const PtyWait &wait)
{
    std::byte next{};
    if (wait.committed >= wait.count || !gather_pty_suffix(wait.space, {&next, 1}, wait)) return true;
    const auto progress = kernel.scheduler().pty_write_progress(wait.description, next);
    return !progress || progress.value();
}
i64 resume_pty_io(Kernel &kernel, sched::ProcessControlBlock &process,
                  memory::ObjectHandle object, bool &complete)
{
    auto resolved = pty_wait_objects.resolve(object);
    if (!resolved) { runtime_failure = resolved.status(); complete = false; return error(resolved.status()); }
    auto &wait = *resolved.value();
    auto *thread = kernel.scheduler().find_thread(wait.pid, wait.tid);
    if (!thread || !pty_owner_current(process, *thread, wait) ||
        thread->wait_registration.ticket() != wait.ticket || thread->wait_registration.active())
    { cancel_pty_wait(kernel, object); complete = false; return -125; }
    const auto completion = thread->wait_registration.completion();
    // A VTIME expiry retries the read with the timer expired (n_tty_read).
    const bool timer_expired = completion == sched::WaitCompletion::timeout && !wait.writing && wait.deadline;
    if (completion != sched::WaitCompletion::ready && !timer_expired)
    {
        const i64 failure = completion == sched::WaitCompletion::interrupted ? -4 : -125;
        const auto result = pty_terminal_result(wait, failure);
        cancel_pty_wait(kernel, object, "interrupted", result); return result;
    }
    if (timer_expired) wait.deadline = 0;
    // A continue ended the wait: the slave call runs its job check again. A
    // write that already moved bytes returns them unchecked, foreground or
    // not (Linux n_tty_write returned b - buf when the stop interrupted it,
    // and a partial write is never restarted).
    if (wait.endpoint.side == sched::private_pty::Side::slave &&
        thread->terminal_gate_mark != process.job_control().continue_generation)
    {
        if (wait.writing && wait.committed)
        {
            const auto result = pty_terminal_result(wait, -5);
            cancel_pty_wait(kernel, object, "job-control", result);
            return result;
        }
        if (const auto gate = tty_check(kernel, process, *thread, pty_terminal_ref(wait.endpoint),
                                        wait.writing ? TtyAccess::write : TtyAccess::read); gate != TtyGate::go)
        {
            if (gate == TtyGate::io_error)
            {
                cancel_pty_wait(kernel, object, "job-control", -5);
                return -5;
            }
            // The park takes over the frame and its origin capture.
            const auto fd = wait.fd;
            const auto ref = pty_terminal_ref(wait.endpoint);
            wait.origin = 0;
            cancel_pty_wait(kernel, object, "job-control", -11);
            bool parked = false;
            const auto status = tty_park(kernel, process, *thread, ref, fd, parked);
            complete = !parked;
            return error(status);
        }
    }
    const auto before = wait;
    auto transferred = wait.writing ? attempt_pty_write(kernel, process, wait)
                                    : attempt_pty_io(kernel, process, wait, timer_expired);
    const auto *description = kernel.scheduler().open_description(wait.description);
    const bool blocking = description && !(description->flags & posix::o_NONBLOCK);
    if (blocking && ((transferred && wait.writing && wait.committed < wait.count) ||
        (!transferred && transferred.status().code() == StatusCode::would_block)))
    {
        if (auto status = pty_arm_timer(kernel, wait); !status)
        { const auto result = pty_terminal_result(wait, error(status)); cancel_pty_wait(kernel, object, "error", result); return result; }
        auto registration = kernel.scheduler().register_current_wait(pty_wait_reason(wait.writing), pty_wait_key(wait.endpoint));
        if (!registration)
        { const auto result = pty_terminal_result(wait, error(registration.status())); cancel_pty_wait(kernel, object, "error", result); return result; }
        wait.ticket = registration.value();
        thread->terminal_gate_mark = process.job_control().continue_generation;
        pty_rebind(process, *thread, object, before, wait);
        emit_pty_wait(wait, false, "rearm"); complete = false; return -11;
    }
    // The bound origin still names the binding the wait armed with.
    pty_retire_origin(object, before);
    const auto result = pty_terminal_result(wait, transferred ? static_cast<i64>(transferred.value()) : error(transferred.status()));
    cancel_pty_wait(kernel, object, transferred ? (result ? "ready" : "eof") : "error", result);
    return result;
}
i64 begin_pty_io(Kernel &kernel, sched::ProcessControlBlock &process, sched::ThreadId tid,
                 i32 fd, u16 description_id, uptr address, usize count, bool writing,
                 bool &complete, const PipeVectors &vectors)
{
    const auto *description = kernel.scheduler().open_description(description_id);
    auto *thread = kernel.scheduler().find_thread(process.pid(), tid);
    if (!description || !description->references || !description->pty_endpoint || !thread ||
        !(writing ? description->writable : description->readable)) return -9;
    if (!thread->native_active || !thread->syscall_pending || !thread->entry_stack.pool ||
        !validate_entry_stack(entry_stacks, *thread)) return -22;
    if (count > static_cast<usize>(__INTPTR_MAX__) || (!writing && count > sched::private_pty::RawPtyTable<>::queue_capacity)) return -22;
    if (!count) return 0;
    if (!vectors.count && address > ~uptr{0} - count) return -14;
    for (usize index = 0; index < vectors.count; ++index)
        if (vectors.entries[index].base > ~uptr{0} - vectors.entries[index].length) return -14;
    if (description->pty_endpoint.side == sched::private_pty::Side::slave)
    {
        bool parked = false;
        if (auto status = tty_gate(kernel, process, *thread, pty_terminal_ref(description->pty_endpoint),
                                   writing ? TtyAccess::write : TtyAccess::read, fd, parked); !status)
        {
            if (parked) complete = false;
            return error(status);
        }
    }
    PtyWait initial{};
    initial.pid = process.pid(); initial.tid = tid; initial.space = process.resources().native_space;
    initial.lease = thread->entry_stack; initial.frame = thread->native_frame;
    initial.address = address; initial.count = count; initial.fd = fd; initial.description = description_id;
    initial.endpoint = description->pty_endpoint; initial.vectors = vectors; initial.writing = writing;
    initial.personality = process.resources().personality;
    auto transferred = writing ? attempt_pty_write(kernel, process, initial) : attempt_pty_io(kernel, process, initial);
    const bool blocking = !(description->flags & posix::o_NONBLOCK);
    if ((transferred && (!writing || initial.committed == count || !blocking)) ||
        (!transferred && (transferred.status().code() != StatusCode::would_block || !blocking)))
        return pty_terminal_result(initial, transferred ? static_cast<i64>(transferred.value()) : error(transferred.status()));
    if (pty_wait_object(process.pid(), tid).pool) return pty_terminal_result(initial, -22);
    PollWaitIndex *slot = nullptr;
    for (auto &entry : pty_waits) if (!entry.pid) { slot = &entry; break; }
    if (!slot) return pty_terminal_result(initial, -12);
#ifndef OK_KERNEL_FREESTANDING
    if (auto status = activate_pool(); !status) return pty_terminal_result(initial, error(status));
#endif
    auto allocated = pty_wait_objects.create();
    if (!allocated) return pty_terminal_result(initial, error(allocated.status()));
    const auto object = allocated.value();
    auto resolved = pty_wait_objects.resolve(object);
    if (!resolved)
    {
        runtime_failure = resolved.status();
        if (auto status = pty_wait_objects.destroy(object); !status) runtime_failure = status;
        if (auto status = pty_wait_objects.trim_empty_pages(); !status) runtime_failure = status;
        return pty_terminal_result(initial, error(resolved.status()));
    }
    if (auto status = kernel.scheduler().retain_description(description_id); !status)
    { cancel_pty_wait(kernel, object); return pty_terminal_result(initial, error(status)); }
    auto &wait = *resolved.value(); wait = initial;
    if (auto status = pty_arm_timer(kernel, wait); !status)
    { const auto result = pty_terminal_result(wait, error(status)); cancel_pty_wait(kernel, object); return result; }
    auto registration = kernel.scheduler().register_current_wait(pty_wait_reason(writing), pty_wait_key(wait.endpoint));
    if (!registration)
    { const auto result = pty_terminal_result(wait, error(registration.status())); cancel_pty_wait(kernel, object); return result; }
    wait.ticket = registration.value(); wait.request = wait.ticket.generation;
    thread->terminal_gate_mark = process.job_control().continue_generation;
    *slot = {wait.pid, wait.tid, object};
    if (wait.personality == Personality::linux)
    {
        // Bound when armed, so a caught signal can interrupt or restart it.
        const auto origin = syscall_origins.identity(process, *thread);
        if (syscall_origins.live(origin))
        {
            wait.origin = origin.capture;
            const bool bound = writing ? syscall_origins.bind(origin, thread->native_frame, pty_write_origin_binding(object, wait))
                                       : syscall_origins.bind(origin, thread->native_frame, pty_read_origin_binding(object, wait));
            if (!bound) { wait.origin = 0; syscall_origins.revoke(*thread); }
        }
    }
    emit_pty_wait(wait, false, "arm"); complete = false; return -11;
}
} // namespace runtime_detail
} // namespace ok::user::native
