// Raw PTY pair I/O continuations and their owners.
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
void cancel_pty_wait(Kernel &kernel, memory::ObjectHandle object,
                     std::string_view outcome = "cancelled", i64 result = -125)
{
    auto resolved = pty_wait_objects.resolve(object);
    if (!resolved) { runtime_failure = resolved.status(); return; }
    const auto wait = *resolved.value();
    for (auto &entry : pty_waits) if (entry.object == object) entry = {};
    if (wait.ticket.generation)
        static_cast<void>(kernel.scheduler().complete_wait(wait.ticket, sched::WaitCompletion::cancelled));
    if (wait.description != sched::invalid_description)
        if (auto status = kernel.scheduler().release_description(wait.description); !status) runtime_failure = status;
    if (auto status = pty_wait_objects.destroy(object); !status) runtime_failure = status;
    if (auto status = pty_wait_objects.trim_empty_pages(); !status) runtime_failure = status;
    if (wait.ticket.generation) emit_pty_wait(wait, true, outcome, result);
}
} // namespace
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
        process.resources().personality == Personality::native && thread.native_active && thread.syscall_pending &&
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
Result<usize> attempt_pty_io(Kernel &kernel, sched::ProcessControlBlock &process, const PtyWait &wait)
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
            auto prepared = kernel.scheduler().prepare_pty_read(wait.description, wait.count);
            if (!prepared) return prepared.status();
            auto &preview = prepared.value();
            if (preview.endpoint() != wait.endpoint)
            { preview.cancel(); return Status::bad_descriptor("PTY preview endpoint changed"); }
            const auto count = preview.bytes().size();
            if (!pipe_scatter(space, wait.address, preview.bytes(), wait.vectors))
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
    if (readiness.value().hangup) return Status::io_error("PTY peer retired");
    if (!readiness.value().writable) return Status::would_block("PTY outgoing queue is full");
    usize count = wait.count - wait.committed;
    if (count > buffer.size()) count = buffer.size();
    if (count > readiness.value().outgoing_available) count = readiness.value().outgoing_available;
    if (auto status = gather_pty_suffix(space, {buffer.data(), count}, wait); !status) return status;
    return kernel.scheduler().write_pty_description(wait.description, {buffer.data(), count});
}
} // namespace
i64 resume_pty_io(Kernel &kernel, sched::ProcessControlBlock &process,
                  memory::ObjectHandle object, bool &complete)
{
    auto resolved = pty_wait_objects.resolve(object);
    if (!resolved) { runtime_failure = resolved.status(); complete = false; return error(resolved.status()); }
    auto &wait = *resolved.value();
    const auto *thread = kernel.scheduler().find_thread(wait.pid, wait.tid);
    if (!thread || !pty_owner_current(process, *thread, wait) ||
        thread->wait_registration.ticket() != wait.ticket || thread->wait_registration.active())
    { cancel_pty_wait(kernel, object); complete = false; return -125; }
    if (thread->wait_registration.completion() != sched::WaitCompletion::ready)
    {
        const i64 failure = thread->wait_registration.completion() == sched::WaitCompletion::interrupted ? -4 : -125;
        const auto result = pty_terminal_result(wait, failure);
        cancel_pty_wait(kernel, object, "interrupted", result); return result;
    }
    auto transferred = attempt_pty_io(kernel, process, wait);
    if (transferred && wait.writing) wait.committed += transferred.value();
    const auto *description = kernel.scheduler().open_description(wait.description);
    const bool blocking = description && !(description->flags & posix::o_NONBLOCK);
    if (blocking && ((transferred && wait.writing && wait.committed < wait.count) ||
        (!transferred && transferred.status().code() == StatusCode::would_block)))
    {
        auto registration = kernel.scheduler().register_current_wait(pty_wait_reason(wait.writing), pty_wait_key(wait.endpoint));
        if (!registration)
        { const auto result = pty_terminal_result(wait, error(registration.status())); cancel_pty_wait(kernel, object, "error", result); return result; }
        wait.ticket = registration.value(); emit_pty_wait(wait, false, "rearm"); complete = false; return -11;
    }
    const auto result = pty_terminal_result(wait, transferred ? static_cast<i64>(transferred.value()) : error(transferred.status()));
    cancel_pty_wait(kernel, object, transferred ? (result ? "ready" : "eof") : "error", result);
    return result;
}
i64 begin_pty_io(Kernel &kernel, sched::ProcessControlBlock &process, sched::ThreadId tid,
                 i32 fd, u16 description_id, uptr address, usize count, bool writing,
                 bool &complete, const PipeVectors &vectors)
{
    if (process.resources().personality != Personality::native) return -95;
    const auto *description = kernel.scheduler().open_description(description_id);
    const auto *thread = kernel.scheduler().find_thread(process.pid(), tid);
    if (!description || !description->references || !description->pty_endpoint || !thread ||
        !(writing ? description->writable : description->readable)) return -9;
    if (!thread->native_active || !thread->syscall_pending || !thread->entry_stack.pool ||
        !validate_entry_stack(entry_stacks, *thread)) return -22;
    if (count > static_cast<usize>(__INTPTR_MAX__) || (!writing && count > sched::private_pty::RawPtyTable<>::queue_capacity)) return -22;
    if (!count) return 0;
    if (!vectors.count && address > ~uptr{0} - count) return -14;
    for (usize index = 0; index < vectors.count; ++index)
        if (vectors.entries[index].base > ~uptr{0} - vectors.entries[index].length) return -14;
    PtyWait initial{};
    initial.pid = process.pid(); initial.tid = tid; initial.space = process.resources().native_space;
    initial.lease = thread->entry_stack; initial.frame = thread->native_frame;
    initial.address = address; initial.count = count; initial.fd = fd; initial.description = description_id;
    initial.endpoint = description->pty_endpoint; initial.vectors = vectors; initial.writing = writing;
    auto transferred = attempt_pty_io(kernel, process, initial);
    if (transferred && writing) initial.committed = transferred.value();
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
    auto registration = kernel.scheduler().register_current_wait(pty_wait_reason(writing), pty_wait_key(wait.endpoint));
    if (!registration)
    { const auto result = pty_terminal_result(wait, error(registration.status())); cancel_pty_wait(kernel, object); return result; }
    wait.ticket = registration.value(); wait.request = wait.ticket.generation;
    *slot = {wait.pid, wait.tid, object}; emit_pty_wait(wait, false, "arm"); complete = false; return -11;
}
} // namespace runtime_detail
} // namespace ok::user::native
