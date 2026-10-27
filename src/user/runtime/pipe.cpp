// Blocking pipe read/write continuations, including vectored I/O.
#include "private.hpp"

namespace ok::user::native
{
inline namespace runtime_detail
{
SyscallOrigins::PipeBinding pipe_origin_binding(memory::ObjectHandle object, const PipeReadWait &wait)
{
    return {object, wait.ticket, wait.request, wait.fd, wait.output, wait.count, wait.description, wait.pipe, wait.vectors};
}
constinit PipeReadObjects pipe_read_objects{page_pool, pipe_read_capacity};
std::array<PollWaitIndex, pipe_read_capacity> pipe_read_waits{};
SyscallOrigins::PipeWriteBinding pipe_write_origin_binding(memory::ObjectHandle object, const PipeWriteWait &wait)
{
    return {object, wait.ticket, wait.request, wait.fd, wait.input, wait.count, wait.description, wait.pipe, wait.vectors, wait.committed};
}
constinit PipeWriteObjects pipe_write_objects{page_pool, pipe_write_capacity};
std::array<PollWaitIndex, pipe_write_capacity> pipe_write_waits{};
[[maybe_unused]] void emit_pipe_vectors(const PipeVectors &vectors)
{
    if (!vectors.count) return;
    emit(" iovcnt="); emit_number(vectors.count); emit(" vectors=");
    for (usize i = 0; i < vectors.count; ++i)
    {
        if (i) emit(",");
        emit_number(vectors.entries[i].base); emit(":"); emit_number(vectors.entries[i].length);
    }
}
bool pipe_ranges(const hw::AddressSpace &space, uptr address, usize count, const PipeVectors &vectors, bool output)
{
    if (!vectors.count) return hw::user_range(space, address, count, output);
    if (!vectors.staged_count(sched::pipe_capacity + 1)) return false;
    for (usize i = 0; i < vectors.count; ++i)
        if (vectors.entries[i].length && !hw::user_range(space, vectors.entries[i].base, vectors.entries[i].length, output))
            return false;
    return true;
}
namespace
{
Status pipe_gather(const hw::AddressSpace &space, std::span<std::byte> bytes, uptr address, const PipeVectors &vectors)
{
    if (!vectors.count) return hw::copy_from_user(space, bytes.data(), address, bytes.size());
    usize offset = 0;
    for (usize i = 0; i < vectors.count && offset < bytes.size(); ++i)
    {
        const auto length = vectors.entries[i].length < bytes.size() - offset ? vectors.entries[i].length : bytes.size() - offset;
        if (length)
            if (auto status = hw::copy_from_user(space, bytes.data() + offset, vectors.entries[i].base, length); !status) return status;
        offset += length;
    }
    return offset == bytes.size() ? Status::success() : Status::fault("short copied pipe vector input");
}
} // namespace
Status pipe_scatter(const hw::AddressSpace &space, uptr address, std::span<const std::byte> bytes, const PipeVectors &vectors)
{
    if (!vectors.count) return hw::copy_to_user(space, address, bytes.data(), bytes.size());
    usize offset = 0;
    for (usize i = 0; i < vectors.count && offset < bytes.size(); ++i)
    {
        const auto length = vectors.entries[i].length < bytes.size() - offset ? vectors.entries[i].length : bytes.size() - offset;
        if (length)
            if (auto status = hw::copy_to_user(space, vectors.entries[i].base, bytes.data() + offset, length); !status) return status;
        offset += length;
    }
    return offset == bytes.size() ? Status::success() : Status::fault("short copied pipe vector output");
}
memory::ObjectHandle pipe_read_object(sched::ProcessId pid, sched::ThreadId tid)
{
    for (const auto &entry : pipe_read_waits)
        if (entry.pid == pid && entry.tid == tid) return entry.object;
    return {};
}
namespace
{
void emit_pipe_read(const PipeReadWait &wait, bool terminal, std::string_view state, i64 result = 0,
                    bool released = true)
{
#ifdef OK_ENABLE_TEST_POINTS
    emit(wait.vectors.count ? (terminal ? "\nOK_NATIVE_PIPE_READV_COMPLETE pid=" : "\nOK_NATIVE_PIPE_READV_WAIT pid=")
                            : (terminal ? "\nOK_NATIVE_PIPE_READ_COMPLETE pid=" : "\nOK_NATIVE_PIPE_READ_WAIT pid="));
    emit_number(wait.pid); emit(" tid="); emit_number(wait.tid);
    emit(" wait="); emit_number(wait.ticket.generation);
    emit(" request="); emit_number(wait.request);
    emit(wait.vectors.count ? " array=" : " output="); emit_number(wait.output);
    emit(" fd="); emit_number(static_cast<u32>(wait.fd));
    emit(" description="); emit_number(wait.description);
    emit(" pipe="); emit_number(wait.pipe); emit(" bytes="); emit_number(wait.count);
    emit_pipe_vectors(wait.vectors);
    if (terminal)
    {
        emit(" result=");
        if (result < 0) { emit("-"); emit_number(u64{0} - static_cast<u64>(result)); }
        else emit_number(static_cast<u64>(result));
        emit(" outcome="); emit(state); emit(released ? " pinned=0" : " pinned=1");
    }
    else { emit(" stage="); emit(state); }
    emit(" live="); emit_number(pipe_read_objects.accounting().live_objects); emit("\n");
#else
    static_cast<void>(wait); static_cast<void>(terminal); static_cast<void>(state);
    static_cast<void>(result); static_cast<void>(released);
#endif
}
} // namespace
void cancel_pipe_read(Kernel &kernel, memory::ObjectHandle object,
                      std::string_view outcome, i64 result)
{
    auto resolved = pipe_read_objects.resolve(object);
    if (!resolved) { runtime_failure = resolved.status(); return; }
    const auto wait = *resolved.value();
    syscall_origins.retire_pipe(wait.pid, wait.tid, pipe_origin_binding(object, wait));
    for (auto &entry : pipe_read_waits)
        if (entry.object == object) entry = {};
    // An old object's teardown cannot complete or cancel a replacement wait.
    if (wait.ticket.generation)
        static_cast<void>(kernel.scheduler().complete_wait(wait.ticket, sched::WaitCompletion::cancelled));
    bool released = true;
    if (wait.description != sched::invalid_description)
        if (auto status = kernel.scheduler().release_description(wait.description); !status)
        { runtime_failure = status; released = false; }
    if (auto status = pipe_read_objects.destroy(object); !status) runtime_failure = status;
    if (auto status = pipe_read_objects.trim_empty_pages(); !status) runtime_failure = status;
    // Only published waits have a matching terminal record.
    if (wait.ticket.generation) emit_pipe_read(wait, true, outcome, result, released);
}
void cancel_pipe_read(Kernel &kernel, sched::ProcessId pid)
{
    for (const auto &entry : pipe_read_waits)
        if (entry.pid == pid) cancel_pipe_read(kernel, entry.object);
}
void prune_pipe_reads(Kernel &kernel)
{
    for (const auto &entry : pipe_read_waits)
    {
        if (!entry.pid) continue;
        const auto object = entry.object;
        auto resolved = pipe_read_objects.resolve(object);
        if (!resolved) { runtime_failure = resolved.status(); continue; }
        const auto &wait = *resolved.value();
        const auto *process = kernel.scheduler().find(wait.pid);
        const auto *thread = kernel.scheduler().find_thread(wait.pid, wait.tid);
        if (!process || !thread || !thread->native_active || !thread->syscall_pending ||
            process->state() == sched::ProcessState::zombie || process->state() == sched::ProcessState::exited ||
            thread->state == sched::ProcessState::exited || thread->wait_registration.ticket() != wait.ticket ||
            (thread->wait_registration.active() &&
             (thread->state != sched::ProcessState::blocked || thread->wait_reason != sched::ThreadWaitReason::pipe_read ||
              thread->wait_key != wait.pipe)))
            cancel_pipe_read(kernel, object);
    }
}
i64 resume_pipe_read(Kernel &kernel, sched::ProcessControlBlock &process,
                     memory::ObjectHandle object, bool &complete)
{
    auto resolved = pipe_read_objects.resolve(object);
    if (!resolved) { runtime_failure = resolved.status(); return error(resolved.status()); }
    auto &wait = *resolved.value();
    const auto *thread = kernel.scheduler().find_thread(wait.pid, wait.tid);
    if (!thread || thread->wait_registration.ticket() != wait.ticket || thread->wait_registration.active())
    { cancel_pipe_read(kernel, object); return -125; }
    if (thread->wait_registration.completion() != sched::WaitCompletion::ready)
    {
        // Recipient-side signal interruption retires the object before resume.
        // No restart/EINTR policy is synthesized from an unrelated completion.
        cancel_pipe_read(kernel, object);
        return -125;
    }
    const auto *description = kernel.scheduler().open_description(wait.description);
    if (!description || !description->readable || description->pipe != wait.pipe)
    { cancel_pipe_read(kernel, object, "error", -9); return -9; }
    auto &space = process.resources().native_space;
    // Mapping changes by a peer must be observed before consuming pipe bytes.
    if (!pipe_ranges(space, wait.output, wait.count, wait.vectors, true))
    { cancel_pipe_read(kernel, object, "efault", -14); return -14; }
    std::array<std::byte, sched::pipe_capacity> buffer{};
    auto read = kernel.scheduler().read_pipe(wait.pipe, {buffer.data(), wait.count}, true);
    if (!read && read.status().code() == StatusCode::would_block && !(description->flags & posix::o_NONBLOCK))
    {
        auto registration = kernel.scheduler().register_current_wait(sched::ThreadWaitReason::pipe_read, wait.pipe);
        if (!registration)
        {
            const auto result = error(registration.status());
            cancel_pipe_read(kernel, object, "error", result);
            return result;
        }
        const auto previous_binding = pipe_origin_binding(object, wait);
        wait.ticket = registration.value();
        const auto origin = syscall_origins.identity(process, *thread);
        if (origin.capture &&
            !syscall_origins.rearm(origin, thread->native_frame, previous_binding, pipe_origin_binding(object, wait)))
            syscall_origins.revoke(*thread);
        emit_pipe_read(wait, false, "rearm");
        complete = false;
        return -11;
    }
    i64 result = read ? static_cast<i64>(read.value()) : error(read.status());
    if (read && !pipe_scatter(space, wait.output, {buffer.data(), read.value()}, wait.vectors)) result = -14;
    cancel_pipe_read(kernel, object, result > 0 ? "ready" : result == 0 ? "eof" : result == -14 ? "efault" : "error", result);
    return result;
}
i64 begin_pipe_read(Kernel &kernel, sched::ProcessControlBlock &process, sched::ThreadId tid,
                    i32 fd, u16 description_id, uptr output, usize count, bool &complete, const PipeVectors &vectors)
{
    auto &space = process.resources().native_space;
    if (count > sched::pipe_capacity || (vectors.count && vectors.staged_count(sched::pipe_capacity) != count)) return -22;
    if (!pipe_ranges(space, output, count, vectors, true)) return -14;
    const auto *description = kernel.scheduler().open_description(description_id);
    if (!description || !description->readable) return -9;
    std::array<std::byte, sched::pipe_capacity> buffer{};
    auto read = kernel.scheduler().read_pipe(description->pipe, {buffer.data(), count}, true);
    if (read)
        return pipe_scatter(space, output, {buffer.data(), read.value()}, vectors) ? static_cast<i64>(read.value()) : -14;
    if (read.status().code() != StatusCode::would_block || (description->flags & posix::o_NONBLOCK))
        return error(read.status());
    PollWaitIndex *slot = nullptr;
    for (auto &entry : pipe_read_waits)
        if (!entry.pid) { slot = &entry; break; }
    if (!slot) return -12;
#ifndef OK_KERNEL_FREESTANDING
    if (auto status = activate_pool(); !status) return error(status);
#endif
    auto allocated = pipe_read_objects.create();
    if (!allocated) return error(allocated.status());
    const auto object = allocated.value();
    auto resolved = pipe_read_objects.resolve(object);
    if (!resolved) { runtime_failure = resolved.status(); return error(resolved.status()); }
    auto &wait = *resolved.value();
    if (auto status = kernel.scheduler().retain_description(description_id); !status)
    { cancel_pipe_read(kernel, object); return error(status); }
    wait.pid = process.pid(); wait.tid = tid; wait.output = output; wait.count = count; wait.vectors = vectors;
    wait.fd = fd; wait.description = description_id; wait.pipe = description->pipe;
    auto registration = kernel.scheduler().register_current_wait(sched::ThreadWaitReason::pipe_read, wait.pipe);
    if (!registration)
    { cancel_pipe_read(kernel, object); return error(registration.status()); }
    wait.ticket = registration.value();
    wait.request = wait.ticket.generation;
    *slot = {wait.pid, wait.tid, object};
    if (auto *thread = kernel.scheduler().find_thread(wait.pid, wait.tid))
    {
        const auto origin = syscall_origins.identity(process, *thread);
        if (syscall_origins.live(origin) &&
            !syscall_origins.bind(origin, thread->native_frame, pipe_origin_binding(object, wait)))
            syscall_origins.revoke(*thread);
    }
    emit_pipe_read(wait, false, "arm");
#if defined(OK_PRIVATE_SIGNAL_DELIVERY_FIXTURE) && defined(OK_ENABLE_TEST_POINTS)
    if (auto *thread = kernel.scheduler().find_thread(wait.pid, wait.tid))
    {
        const auto origin = syscall_origins.identity(process, *thread);
        const auto permit = syscall_origins.prepare_restart(origin, thread->native_frame, pipe_origin_binding(object, wait));
        if (permit)
        {
            emit(wait.vectors.count ? "OK_NATIVE_PIPE_READV_ORIGIN pid=" : "OK_NATIVE_PIPE_ORIGIN pid="); emit_number(wait.pid); emit(" tid="); emit_number(wait.tid);
            emit(" request="); emit_number(wait.request); emit(" wait="); emit_number(wait.ticket.generation);
            emit(" origin="); emit_number(origin.capture); emit(" pc="); emit_number(thread->native_frame.pc);
            emit(" syscall="); emit_number(*lu::Codec<lu::configured_native_profile()>::syscall_number(wait.vectors.count ? lu::Operation::readv : lu::Operation::read));
            emit(" fd="); emit_number(static_cast<u32>(wait.fd)); emit(wait.vectors.count ? " array=" : " output="); emit_number(wait.output);
            emit(" bytes="); emit_number(wait.count); emit_pipe_vectors(wait.vectors); emit("\n");
        }
    }
#endif
    complete = false;
    return -11;
}

memory::ObjectHandle pipe_write_object(sched::ProcessId pid, sched::ThreadId tid)
{
    for (const auto &entry : pipe_write_waits)
        if (entry.pid == pid && entry.tid == tid) return entry.object;
    return {};
}
namespace
{
void emit_pipe_write(const PipeWriteWait &wait, bool terminal, std::string_view state, i64 result = 0,
                    bool released = true)
{
#ifdef OK_ENABLE_TEST_POINTS
    if (wait.count > sched::pipe_capacity)
        emit(wait.vectors.count ? (terminal ? "\nOK_NATIVE_PIPE_WRITEV_LARGE_COMPLETE pid=" : "\nOK_NATIVE_PIPE_WRITEV_LARGE_WAIT pid=")
                                : (terminal ? "\nOK_NATIVE_PIPE_WRITE_LARGE_COMPLETE pid=" : "\nOK_NATIVE_PIPE_WRITE_LARGE_WAIT pid="));
    else
        emit(wait.vectors.count ? (terminal ? "\nOK_NATIVE_PIPE_WRITEV_COMPLETE pid=" : "\nOK_NATIVE_PIPE_WRITEV_WAIT pid=")
                            : (terminal ? "\nOK_NATIVE_PIPE_WRITE_COMPLETE pid=" : "\nOK_NATIVE_PIPE_WRITE_WAIT pid="));
    emit_number(wait.pid); emit(" tid="); emit_number(wait.tid);
    emit(" wait="); emit_number(wait.ticket.generation);
    emit(" request="); emit_number(wait.request);
    emit(wait.vectors.count ? " array=" : " input="); emit_number(wait.input);
    emit(" fd="); emit_number(static_cast<u32>(wait.fd));
    emit(" description="); emit_number(wait.description);
    emit(" pipe="); emit_number(wait.pipe); emit(" bytes="); emit_number(wait.count);
    emit_pipe_vectors(wait.vectors);
    if (wait.count > sched::pipe_capacity) { emit(" committed="); emit_number(wait.committed); }
    if (terminal)
    {
        emit(" result=");
        if (result < 0) { emit("-"); emit_number(u64{0} - static_cast<u64>(result)); }
        else emit_number(static_cast<u64>(result));
        emit(" outcome="); emit(state); emit(released ? " pinned=0" : " pinned=1");
    }
    else { emit(" stage="); emit(state); }
    emit(" live="); emit_number(pipe_write_objects.accounting().live_objects); emit("\n");
#else
    static_cast<void>(wait); static_cast<void>(terminal); static_cast<void>(state);
    static_cast<void>(result); static_cast<void>(released);
#endif
}
} // namespace
void cancel_pipe_write(Kernel &kernel, memory::ObjectHandle object,
                      std::string_view outcome, i64 result)
{
    auto resolved = pipe_write_objects.resolve(object);
    if (!resolved) { runtime_failure = resolved.status(); return; }
    const auto wait = *resolved.value();
    syscall_origins.retire_pipe_write(wait.pid, wait.tid, pipe_write_origin_binding(object, wait));
    for (auto &entry : pipe_write_waits)
        if (entry.object == object) entry = {};
    // An old object's teardown cannot complete or cancel a replacement wait.
    if (wait.ticket.generation)
        static_cast<void>(kernel.scheduler().complete_wait(wait.ticket, sched::WaitCompletion::cancelled));
    bool released = true;
    if (wait.description != sched::invalid_description)
        if (auto status = kernel.scheduler().release_description(wait.description); !status)
        { runtime_failure = status; released = false; }
    if (auto status = pipe_write_objects.destroy(object); !status) runtime_failure = status;
    if (auto status = pipe_write_objects.trim_empty_pages(); !status) runtime_failure = status;
    // Only published waits have a matching terminal record.
    if (wait.ticket.generation) emit_pipe_write(wait, true, outcome, result, released);
}
void cancel_pipe_write(Kernel &kernel, sched::ProcessId pid)
{
    for (const auto &entry : pipe_write_waits)
        if (entry.pid == pid) cancel_pipe_write(kernel, entry.object);
}
void prune_pipe_writes(Kernel &kernel)
{
    for (const auto &entry : pipe_write_waits)
    {
        if (!entry.pid) continue;
        const auto object = entry.object;
        auto resolved = pipe_write_objects.resolve(object);
        if (!resolved) { runtime_failure = resolved.status(); continue; }
        const auto &wait = *resolved.value();
        const auto *process = kernel.scheduler().find(wait.pid);
        const auto *thread = kernel.scheduler().find_thread(wait.pid, wait.tid);
        if (!process || !thread || !thread->native_active || !thread->syscall_pending ||
            process->state() == sched::ProcessState::zombie || process->state() == sched::ProcessState::exited ||
            thread->state == sched::ProcessState::exited || thread->wait_registration.ticket() != wait.ticket ||
            (thread->wait_registration.active() &&
             (thread->state != sched::ProcessState::blocked || thread->wait_reason != sched::ThreadWaitReason::pipe_write ||
              thread->wait_key != wait.pipe)))
            cancel_pipe_write(kernel, object);
    }
}
namespace
{
// Validate only the not-yet-published suffix; consumed mappings may have gone away.
Status gather_pipe_write(const hw::AddressSpace &space, std::span<std::byte> bytes,
                         const PipeWriteWait &wait)
{
    if (!wait.vectors.count)
    {
        if (wait.input > ~uptr{0} - wait.committed) return Status::fault("pipe input overflow");
        return hw::copy_from_user(space, bytes.data(), wait.input + wait.committed, bytes.size());
    }
    usize skip = wait.committed, copied = 0;
    for (usize i = 0; i < wait.vectors.count && copied < bytes.size(); ++i)
    {
        const auto &vector = wait.vectors.entries[i];
        if (skip >= vector.length) { skip -= vector.length; continue; }
        const auto available = vector.length - skip;
        const auto length = available < bytes.size() - copied ? available : bytes.size() - copied;
        if (vector.base > ~uptr{0} - skip) return Status::fault("pipe vector input overflow");
        if (auto status = hw::copy_from_user(space, bytes.data() + copied, vector.base + skip, length); !status)
            return status;
        copied += length; skip = 0;
    }
    return copied == bytes.size() ? Status::success() : Status::fault("short pipe write suffix");
}
Result<usize> attempt_pipe_write(Kernel &kernel, sched::ProcessControlBlock &process, const PipeWriteWait &wait)
{
    auto &space = process.resources().native_space;
    const auto *description = kernel.scheduler().open_description(wait.description);
    const bool atomic = wait.count <= sched::pipe_capacity && description &&
                        description->unix_write_pipe == sched::invalid_description;
    usize staged = wait.count - wait.committed;
    if (atomic)
    {
        // Preserve existing small-write validation and EFAULT precedence.
        if (!pipe_ranges(space, wait.input, wait.count, wait.vectors, false))
            return Status::fault("invalid atomic pipe input");
    }
    else
    {
        const auto ready = kernel.scheduler().pipe_readiness(wait.pipe);
        if (!ready) return ready.status();
        if (!ready.value().readers) return Status::broken_pipe("pipe has no readers");
        if (!ready.value().available) return Status::would_block("pipe buffer is full");
        if (staged > ready.value().available) staged = ready.value().available;
    }
    std::array<std::byte, sched::pipe_capacity> buffer{};
    const auto copied = atomic ? pipe_gather(space, {buffer.data(), staged}, wait.input, wait.vectors)
                               : gather_pipe_write(space, {buffer.data(), staged}, wait);
    if (!copied) return copied;
    return kernel.scheduler().write_pipe_fragment(wait.pipe, {buffer.data(), staged}, atomic);
}
} // namespace
i64 pipe_write_terminal_result(const PipeWriteWait &wait, i64 failure)
{ return wait.committed ? static_cast<i64>(wait.committed) : failure; }
namespace
{
void emit_pipe_write_origin(Kernel &kernel, sched::ProcessControlBlock &process,
                            memory::ObjectHandle object, const PipeWriteWait &wait)
{
#if defined(OK_PRIVATE_SIGNAL_DELIVERY_FIXTURE) && defined(OK_ENABLE_TEST_POINTS)
    if (auto *thread = kernel.scheduler().find_thread(wait.pid, wait.tid))
    {
        const auto origin = syscall_origins.identity(process, *thread);
        if (syscall_origins.validate_bound(origin, thread->native_frame, pipe_write_origin_binding(object, wait)))
        {
            if (wait.count > sched::pipe_capacity)
                emit(wait.vectors.count ? "OK_NATIVE_PIPE_WRITEV_LARGE_ORIGIN pid=" : "OK_NATIVE_PIPE_WRITE_LARGE_ORIGIN pid=");
            else emit(wait.vectors.count ? "OK_NATIVE_PIPE_WRITEV_ORIGIN pid=" : "OK_NATIVE_PIPE_WRITE_ORIGIN pid=");
            emit_number(wait.pid); emit(" tid="); emit_number(wait.tid);
            emit(" request="); emit_number(wait.request); emit(" wait="); emit_number(wait.ticket.generation);
            emit(" origin="); emit_number(origin.capture); emit(" pc="); emit_number(thread->native_frame.pc);
            emit(" syscall="); emit_number(*lu::Codec<lu::configured_native_profile()>::syscall_number(wait.vectors.count ? lu::Operation::writev : lu::Operation::write));
            emit(" fd="); emit_number(static_cast<u32>(wait.fd)); emit(wait.vectors.count ? " array=" : " input="); emit_number(wait.input);
            emit(" bytes="); emit_number(wait.count); emit_pipe_vectors(wait.vectors);
            if (wait.count > sched::pipe_capacity) { emit(" committed="); emit_number(wait.committed); }
            emit("\n");
        }
    }
#else
    static_cast<void>(kernel); static_cast<void>(process); static_cast<void>(object); static_cast<void>(wait);
#endif
}
} // namespace
i64 resume_pipe_write(Kernel &kernel, sched::ProcessControlBlock &process,
                     memory::ObjectHandle object, bool &complete)
{
    auto resolved = pipe_write_objects.resolve(object);
    if (!resolved) { runtime_failure = resolved.status(); return error(resolved.status()); }
    auto &wait = *resolved.value();
    auto *thread = kernel.scheduler().find_thread(wait.pid, wait.tid);
    if (process.pid() != wait.pid || !thread || !thread->native_active || !thread->syscall_pending ||
        thread->wait_registration.ticket() != wait.ticket || thread->wait_registration.active())
    { cancel_pipe_write(kernel, object); complete = false; return -125; }
    const auto admitted = syscall_origins.identity(process, *thread);
    const bool captured_owner = admitted.capture == wait.origin_capture;
    if (thread->wait_registration.completion() != sched::WaitCompletion::ready)
    {
        const bool owned = captured_owner && syscall_origins.validate_cancelled_pipe_write(admitted, thread->native_frame,
                                                                                           pipe_write_origin_binding(object, wait));
        const i64 result = owned ? pipe_write_terminal_result(wait, -125) : -125;
        const bool unpublished = !owned && (wait.committed || wait.origin_capture);
        cancel_pipe_write(kernel, object, "cancelled", result);
        if (unpublished) complete = false;
        return result;
    }
    // A readiness wake carries no authority to complete a replaced syscall/image.
    // Keep originless bootstrap/model calls distinct from a revoked admission.
    if (wait.origin_capture && (!captured_owner ||
        !syscall_origins.validate_bound(admitted, thread->native_frame, pipe_write_origin_binding(object, wait))))
    { cancel_pipe_write(kernel, object); complete = false; return -125; }
    const auto *description = kernel.scheduler().open_description(wait.description);
    if (!description || !description->writable ||
        (description->unix_write_pipe != sched::invalid_description
             ? description->unix_write_pipe : description->pipe) != wait.pipe)
    {
        const auto result = pipe_write_terminal_result(wait, -9);
        cancel_pipe_write(kernel, object, "error", result); return result;
    }
    const auto previous_binding = pipe_write_origin_binding(object, wait);
    auto write = attempt_pipe_write(kernel, process, wait);
    if (write)
    {
        wait.committed += write.value();
        const auto origin = syscall_origins.identity(process, *thread);
        if (origin.capture && !syscall_origins.advance_pipe_write(origin, thread->native_frame,
                previous_binding, pipe_write_origin_binding(object, wait))) syscall_origins.revoke(*thread);
    }
    const bool blocking = !(description->flags & posix::o_NONBLOCK);
    if ((write && wait.committed < wait.count && blocking) ||
        (!write && write.status().code() == StatusCode::would_block && blocking))
    {
        const auto before_rearm = pipe_write_origin_binding(object, wait);
        auto registration = kernel.scheduler().register_current_wait(sched::ThreadWaitReason::pipe_write, wait.pipe);
        if (!registration)
        {
            const auto result = pipe_write_terminal_result(wait, error(registration.status()));
            cancel_pipe_write(kernel, object, "error", result); return result;
        }
        wait.ticket = registration.value();
        const auto origin = syscall_origins.identity(process, *thread);
        if (origin.capture && !syscall_origins.rearm(origin, thread->native_frame, before_rearm, pipe_write_origin_binding(object, wait)))
            syscall_origins.revoke(*thread);
        emit_pipe_write(wait, false, "rearm"); complete = false; return -11;
    }
    const i64 result = pipe_write_terminal_result(wait, write ? 0 : error(write.status()));
    const auto tid = wait.tid;
    const bool broken = !write && write.status().code() == StatusCode::broken_pipe;
    cancel_pipe_write(kernel, object, write ? "ready" : write.status().code() == StatusCode::fault ? "efault" : "error", result);
    if (broken) queue_pipe_sigpipe(kernel, process, tid);
    return result;
}
i64 begin_pipe_write(Kernel &kernel, sched::ProcessControlBlock &process, sched::ThreadId tid,
                    i32 fd, u16 description_id, uptr input, usize count, bool &complete, const PipeVectors &vectors)
{
    if (count > static_cast<usize>(__INTPTR_MAX__) ||
        (vectors.count && vectors.staged_count(static_cast<usize>(__INTPTR_MAX__)) != count)) return -22;
    if (!vectors.count && input > ~uptr{0} - count) return -14;
    for (usize i = 0; i < vectors.count; ++i)
        if (vectors.entries[i].base > ~uptr{0} - vectors.entries[i].length) return -14;
    const auto *description = kernel.scheduler().open_description(description_id);
    if (!description || !description->writable) return -9;
    if (!count) return 0;
    PipeWriteWait initial{};
    initial.pid = process.pid(); initial.tid = tid; initial.input = input; initial.count = count; initial.vectors = vectors;
    initial.fd = fd; initial.description = description_id;
    initial.pipe = description->unix_write_pipe != sched::invalid_description
                       ? description->unix_write_pipe : description->pipe;
    auto write = attempt_pipe_write(kernel, process, initial);
    if (write) initial.committed = write.value();
    const bool blocking = !(description->flags & posix::o_NONBLOCK);
    if (!write && write.status().code() == StatusCode::broken_pipe) queue_pipe_sigpipe(kernel, process, tid);
    if ((write && (initial.committed == count || !blocking)) ||
        (!write && (write.status().code() != StatusCode::would_block || !blocking)))
        return pipe_write_terminal_result(initial, write ? 0 : error(write.status()));
    PollWaitIndex *slot = nullptr;
    for (auto &entry : pipe_write_waits) if (!entry.pid) { slot = &entry; break; }
    if (!slot) return pipe_write_terminal_result(initial, -12);
#ifndef OK_KERNEL_FREESTANDING
    if (auto status = activate_pool(); !status) return pipe_write_terminal_result(initial, error(status));
#endif
    auto allocated = pipe_write_objects.create();
    if (!allocated) return pipe_write_terminal_result(initial, error(allocated.status()));
    const auto object = allocated.value();
    auto resolved = pipe_write_objects.resolve(object);
    if (!resolved) { runtime_failure = resolved.status(); return pipe_write_terminal_result(initial, error(resolved.status())); }
    if (auto status = kernel.scheduler().retain_description(description_id); !status)
    { cancel_pipe_write(kernel, object); return pipe_write_terminal_result(initial, error(status)); }
    auto &wait = *resolved.value(); wait = initial;
    auto registration = kernel.scheduler().register_current_wait(sched::ThreadWaitReason::pipe_write, wait.pipe);
    if (!registration)
    { const auto result = pipe_write_terminal_result(wait, error(registration.status())); cancel_pipe_write(kernel, object); return result; }
    wait.ticket = registration.value(); wait.request = wait.ticket.generation;
    *slot = {wait.pid, wait.tid, object};
    if (auto *thread = kernel.scheduler().find_thread(wait.pid, wait.tid))
    {
        const auto origin = syscall_origins.identity(process, *thread);
        wait.origin_capture = origin.capture;
        if (syscall_origins.live(origin) && !syscall_origins.bind(origin, thread->native_frame, pipe_write_origin_binding(object, wait)))
            syscall_origins.revoke(*thread);
    }
    emit_pipe_write(wait, false, "arm"); emit_pipe_write_origin(kernel, process, object, wait);
    complete = false; return -11;
}
} // namespace runtime_detail
} // namespace ok::user::native
