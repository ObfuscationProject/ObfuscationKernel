// poll/select readiness waits and wait4 child waits.
#include "private.hpp"

namespace ok::user::native
{
inline namespace runtime_detail
{
constinit PollObjects poll_objects{page_pool, sched::max_processes};
std::array<PollWaitIndex, sched::max_processes> poll_waits{};
SyscallOrigins::ChildBinding child_origin_binding(memory::ObjectHandle object, const ChildWait &wait)
{ return {object, wait.ticket, wait.request, wait.operation}; }
constinit ChildWaitObjects child_wait_objects{page_pool, child_wait_capacity};
std::array<PollWaitIndex, child_wait_capacity> child_waits{};

[[maybe_unused]] memory::ObjectHandle poll_object(sched::ProcessId pid, sched::ThreadId tid)
{
    for (const auto &entry : poll_waits)
        if (entry.pid == pid && entry.tid == tid) return entry.object;
    return {};
}
u64 poll_syscall_number(const hw::UserFrame &frame, bool select_sets)
{
    using Bridge = lu::NativeBridge<lu::configured_native_profile()>;
    using Codec = lu::Codec<lu::configured_native_profile()>;
    const auto call = Bridge::decode_call(frame, 0);
    if (!call) return 0;
    auto is = [&](lu::Operation operation) {
        const auto number = Codec::syscall_number(operation);
        return number && *number == call.value().number;
    };
    const bool matches = select_sets
        ? is(lu::Operation::select) || is(lu::Operation::_newselect) ||
          is(lu::Operation::pselect6) || is(lu::Operation::pselect6_time64)
        : is(lu::Operation::poll) || is(lu::Operation::ppoll) || is(lu::Operation::ppoll_time64);
    return matches ? call.value().number : 0;
}
namespace
{
void emit_poll_arm(const PollWait &wait)
{
#if defined(OK_PRIVATE_SIGNAL_DELIVERY_FIXTURE) && defined(OK_ENABLE_TEST_POINTS)
    if (!wait.syscall) return;
    emit("OK_NATIVE_POLL_ARM pid="); emit_number(wait.pid); emit(" tid="); emit_number(wait.tid);
    emit(" wait="); emit_number(wait.ticket.generation); emit(" pc="); emit_number(wait.resume);
    emit(" syscall="); emit_number(wait.syscall); emit(wait.select_sets ? " kind=select" : " kind=poll");
    emit(" count="); emit_number(wait.count); emit(" deadline="); emit_number(wait.timeout.deadline());
    emit(" live="); emit_number(poll_objects.accounting().live_objects); emit("\n");
#else
    static_cast<void>(wait);
#endif
}
} // namespace
void cancel_poll(Kernel &kernel, memory::ObjectHandle object, std::string_view reason, i64 result)
{
    auto found = poll_objects.resolve(object);
    if (!found)
    {
        runtime_failure = found.status();
        return;
    }
    const auto wait = *found.value();
#ifdef OK_PRIVATE_POLL_MASK
    if (auto *owner = kernel.scheduler().find_thread(wait.pid, wait.tid))
        poll_signal_masks.complete(*owner, object, wait.ticket, false);
#endif
    // Remove membership before dropping any references or the object storage.
    for (auto &entry : poll_waits)
        if (entry.pid == wait.pid && entry.tid == wait.tid && entry.object == object)
            entry = {};
    if (wait.ticket.generation)
        static_cast<void>(kernel.scheduler().complete_wait(wait.ticket, sched::WaitCompletion::cancelled));
    for (usize i = 0; i < wait.count; ++i)
        if (wait.descriptions[i] != sched::invalid_description)
            if (auto status = kernel.scheduler().release_description(wait.descriptions[i]); !status)
                runtime_failure = status;
    if (auto status = poll_objects.destroy(object); !status) runtime_failure = status;
    if (auto status = poll_objects.trim_empty_pages(); !status) runtime_failure = status;
#if defined(OK_PRIVATE_SIGNAL_DELIVERY_FIXTURE) && defined(OK_ENABLE_TEST_POINTS)
    if (wait.syscall)
    {
        emit("OK_NATIVE_POLL_RETIRE pid="); emit_number(wait.pid); emit(" tid="); emit_number(wait.tid);
        emit(" wait="); emit_number(wait.ticket.generation); emit(" reason="); emit(reason); emit(" result=");
        if (result < 0) { emit("-"); emit_number(static_cast<u64>(-(result + 1)) + 1); }
        else emit_number(static_cast<u64>(result));
        emit(" live="); emit_number(poll_objects.accounting().live_objects); emit("\n");
    }
#else
    static_cast<void>(reason); static_cast<void>(result);
#endif
}

void cancel_poll(Kernel &kernel, sched::ProcessId pid)
{
    for (const auto &entry : poll_waits)
        if (entry.pid == pid)
            cancel_poll(kernel, entry.object);
}

template <posix::ReadinessPolicy Policy>
Result<posix::DescriptorReadiness> retained_description_readiness(
    Kernel &kernel, u16 id, posix::ReadyEvents requested)
{
    const auto *description = kernel.scheduler().open_description(id);
    if (!description || !description->references)
        return posix::DescriptorReadiness{.events = posix::ReadyEvents::invalid};
    if (!description->native_udp_handle)
        return kernel.posix().poll_description<Policy>(id, requested);
    const auto joined = kernel.native_udp_executor().readiness(
        kernel.native_udp(), description->native_udp_handle);
    return net::native_udp_descriptor_readiness(joined, description->readable,
                                               description->writable, requested);
}

Result<usize> poll_ready(Kernel &kernel, PollWait &wait)
{
    usize count = 0;
    wait.ready_sets = {};
    for (usize i = 0; i < wait.count; ++i)
    {
        auto &entry = wait.fds[i];
        entry.revents = 0;
        if (entry.fd < 0)
            continue;
        if (wait.descriptions[i] == sched::invalid_description)
            entry.revents = OK_POLLNVAL;
        else
        {
            const auto requested = static_cast<u16>(entry.events);
            u16 canonical{};
            if (requested & (OK_POLLIN | wait.read_normal)) canonical |= OK_POLLIN;
            if (requested & (OK_POLLOUT | wait.write_normal)) canonical |= OK_POLLOUT;
            auto ready = wait.linux_events
                ? retained_description_readiness<posix::ReadinessPolicy::linux>(kernel, wait.descriptions[i], static_cast<posix::ReadyEvents>(canonical))
                : retained_description_readiness<posix::ReadinessPolicy::native>(kernel, wait.descriptions[i], static_cast<posix::ReadyEvents>(canonical));
            if (!ready)
                return ready.status();
            u16 events = static_cast<u16>(ready.value().events);
            if (ready.value().console_input)
            {
                if (!wait.console_epoch || !kernel.scheduler().console_authorized(wait.pid, wait.console_epoch))
                    events |= OK_POLLERR | OK_POLLHUP;
                else if (!kernel.scheduler().console_read_authority(wait.pid, wait.console_epoch))
                    events |= OK_POLLERR;
                else if ((canonical & OK_POLLIN) && kernel.scheduler().console_readable(wait.pid, wait.console_epoch))
                    events |= OK_POLLIN;
            }
            u16 result = events & (OK_POLLERR | OK_POLLHUP | OK_POLLNVAL);
            if (events & OK_POLLIN) result |= requested & (OK_POLLIN | wait.read_normal);
            if (events & OK_POLLOUT) result |= requested & (OK_POLLOUT | wait.write_normal);
            entry.revents = static_cast<i16>(result);
        }
        if (wait.select_sets)
        {
            const auto events = static_cast<u16>(entry.revents);
            if (events & OK_POLLNVAL) return Status::bad_descriptor("select description lost");
            const auto bit = u32{1} << entry.fd;
            // Current providers have no priority source. HUP/ERR never imply
            // exceptional data. Priority support also needs canonical event
            // admission and folding before a future provider can supply it.
            const std::array<bool, 3> ready{bool(events & (OK_POLLIN | OK_POLLHUP | OK_POLLERR)),
                bool(events & (OK_POLLOUT | OK_POLLERR)), bool(events & 0x2)};
            for (usize set = 0; set < ready.size(); ++set)
                if (ready[set] && (wait.sets.interests[set] & bit))
                { wait.ready_sets[set] |= bit; ++count; }
        }
        else count += entry.revents != 0;
    }
    return count;
}

bool poll_output(const hw::AddressSpace &space, const PollWait &wait)
{
    if (wait.select_sets)
    {
        if (!wait.sets.count) return true;
        for (usize set = 0; set < wait.sets.output.size(); ++set)
        {
            if (!wait.sets.output[set]) continue;
            std::array<u8, 8> bytes{};
            const auto width = wait.sets.word_bytes;
            for (usize i = 0; i < width; ++i)
                bytes[i] = static_cast<u8>(u64{wait.ready_sets[set]} >> (8 * (wait.big_endian ? width - 1 - i : i)));
            if (!hw::copy_to_user(space, wait.sets.output[set], bytes.data(), width)) return false;
        }
        return true;
    }
    if (!wait.linux_events && !hw::user_range(space, wait.output, wait.count * sizeof(ok_pollfd), true))
        return false;
    for (usize i = 0; i < wait.count; ++i)
    {
        const auto value = static_cast<u16>(wait.fds[i].revents);
        const std::array<u8, 2> bytes{static_cast<u8>(value >> (wait.big_endian ? 8 : 0)),
                                      static_cast<u8>(value >> (wait.big_endian ? 0 : 8))};
        if (!hw::copy_to_user(space, wait.output + i * sizeof(ok_pollfd) + offsetof(ok_pollfd, revents),
                              bytes.data(), bytes.size()))
            return false;
    }
    return true;
}

memory::ObjectHandle child_wait_object(sched::ProcessId pid, sched::ThreadId tid)
{
    for (const auto &entry : child_waits)
        if (entry.pid == pid && entry.tid == tid) return entry.object;
    return {};
}
namespace
{
u64 child_wait_key(const ChildWaitRequest &request)
{ return request.selector == -1 ? 0 : static_cast<u32>(request.selector); }
bool child_wait_output_valid(const sched::ProcessControlBlock &process, const ChildWaitRequest &request)
{
    return !request.siginfo && !request.output ? true :
        hw::user_range(process.resources().native_space, request.output, request.siginfo ? 128 : 4, true);
}
} // namespace
i64 child_wait_probe(Kernel &kernel, sched::ProcessId pid, const ChildWaitRequest &request)
{
    const auto *parent = kernel.scheduler().find(pid);
    if (!parent) return -3;
    if (!child_wait_output_valid(*parent, request)) return -14;
    bool eligible = false;
    for (auto id : parent->children())
    {
        if (request.selector != -1 && id != static_cast<u32>(request.selector)) continue;
        const auto *child = kernel.scheduler().find(id);
        if (!child || child->parent_pid() != pid) continue;
        eligible = true;
        if (child->state() == sched::ProcessState::zombie || child->state() == sched::ProcessState::exited)
            return static_cast<i64>(id);
    }
    return !eligible ? -10 : request.nohang ? 0 : -11;
}
namespace
{
void emit_child_wait(const ChildWait &wait, std::string_view outcome, i64 result = -11)
{
#ifdef OK_ENABLE_TEST_POINTS
    emit("OK_NATIVE_CHILD_WAIT pid="); emit_number(wait.pid); emit(" tid="); emit_number(wait.tid);
    emit(" request="); emit_number(wait.request); emit(" wait="); emit_number(wait.ticket.generation);
    emit(" selector="); emit_number(static_cast<u32>(wait.operation.selector));
    emit(" output="); emit_number(wait.operation.output); emit(wait.operation.siginfo ? " format=siginfo" : " format=status");
    emit(" outcome="); emit(outcome); emit(" result=");
    if (result < 0) { emit("-"); emit_number(u64{0} - static_cast<u64>(result)); }
    else emit_number(static_cast<u64>(result));
    emit(" live="); emit_number(child_wait_objects.accounting().live_objects); emit("\n");
#else
    static_cast<void>(wait); static_cast<void>(outcome); static_cast<void>(result);
#endif
}
} // namespace
void cancel_child_wait(Kernel &kernel, memory::ObjectHandle object,
                       std::string_view outcome, i64 result)
{
    auto resolved = child_wait_objects.resolve(object);
    if (!resolved) { runtime_failure = resolved.status(); return; }
    const auto wait = *resolved.value();
    syscall_origins.retire_child(wait.pid, wait.tid, child_origin_binding(object, wait));
    for (auto &entry : child_waits) if (entry.object == object) entry = {};
    // A stale object's retirement cannot cancel a newer wait on this thread.
    if (wait.ticket.generation)
        static_cast<void>(kernel.scheduler().complete_wait(wait.ticket, sched::WaitCompletion::cancelled));
    if (auto status = child_wait_objects.destroy(object); !status) runtime_failure = status;
    if (auto status = child_wait_objects.trim_empty_pages(); !status) runtime_failure = status;
    if (wait.ticket.generation) emit_child_wait(wait, outcome, result);
}
void cancel_child_wait(Kernel &kernel, sched::ProcessId pid)
{
    for (const auto &entry : child_waits)
        if (entry.pid == pid) cancel_child_wait(kernel, entry.object);
}
void prune_child_waits(Kernel &kernel)
{
    for (const auto &entry : child_waits)
    {
        if (!entry.pid) continue;
        const auto object = entry.object;
        const auto resolved = child_wait_objects.resolve(object);
        if (!resolved) { runtime_failure = resolved.status(); continue; }
        const auto &wait = *resolved.value();
        const auto *process = kernel.scheduler().find(wait.pid);
        const auto *thread = kernel.scheduler().find_thread(wait.pid, wait.tid);
        if (!process || !thread || !thread->native_active || !thread->syscall_pending ||
            process->state() == sched::ProcessState::zombie || process->state() == sched::ProcessState::exited ||
            thread->state == sched::ProcessState::exited || thread->wait_registration.ticket() != wait.ticket ||
            (thread->wait_registration.active() && (thread->state != sched::ProcessState::blocked ||
             thread->wait_reason != sched::ThreadWaitReason::child || thread->wait_key != child_wait_key(wait.operation))))
        { cancel_child_wait(kernel, object); continue; }
        if (thread->wait_registration.active() && child_wait_probe(kernel, wait.pid, wait.operation) != -11)
            static_cast<void>(kernel.scheduler().complete_wait(wait.ticket, sched::WaitCompletion::ready));
    }
}
namespace
{
i64 complete_child_wait(Kernel &kernel, sched::ProcessId pid, const ChildWaitRequest &request, i64 ready)
{
    if (ready < 0) return ready;
    std::array<u8, 128> bytes{};
    auto field = [&](usize offset, u32 value) {
        for (usize i = 0; i < 4; ++i)
            bytes[offset + i] = static_cast<u8>(value >> (8 * (request.big_endian ? 3 - i : i)));
    };
    if (ready > 0)
    {
        const auto *child = kernel.scheduler().find(static_cast<sched::ProcessId>(ready));
        if (!child || child->parent_pid() != pid) return -10;
        const u32 status = child->wait_status(), uid = child->credentials().uid;
        if (request.siginfo)
        {
            const u32 signal = status & 0x7fU;
            field(0, 17); field(8, signal ? ((status & 0x80U) ? 3U : 2U) : 1U);
            field(12, static_cast<u32>(ready)); field(16, uid);
            field(20, signal ? signal : ((status >> 8) & 0xffU));
        }
        else field(0, status);
        // Reaping compacts canonical process storage. Re-find the parent for
        // output; a checked copy failure is reported even after consumption.
        auto reaped = kernel.scheduler().wait_process(pid, static_cast<sched::ProcessId>(ready));
        if (!reaped) return error(reaped.status());
    }
    const auto *parent = kernel.scheduler().find(pid);
    if (!parent) return -3;
    if (request.siginfo || (ready && request.output))
        if (!hw::copy_to_user(parent->resources().native_space, request.output, bytes.data(), request.siginfo ? 128 : 4))
            return -14;
    return request.siginfo ? 0 : ready;
}
} // namespace
i64 resume_child_wait(Kernel &kernel, sched::ProcessControlBlock &process, memory::ObjectHandle object, bool &complete)
{
    auto resolved = child_wait_objects.resolve(object);
    if (!resolved) { runtime_failure = resolved.status(); return error(resolved.status()); }
    auto &wait = *resolved.value();
    auto *thread = kernel.scheduler().find_thread(wait.pid, wait.tid);
    if (!thread || thread->wait_registration.ticket() != wait.ticket || thread->wait_registration.active() ||
        thread->wait_registration.completion() != sched::WaitCompletion::ready)
    { cancel_child_wait(kernel, object); return -125; }
    const auto ready = child_wait_probe(kernel, wait.pid, wait.operation);
    if (ready == -11)
    {
        // A different waiter may have reaped the wake's child while another
        // eligible child remains. Re-arm the same copied request exactly once.
        const auto before = child_origin_binding(object, wait);
        auto registration = kernel.scheduler().register_current_wait(sched::ThreadWaitReason::child, child_wait_key(wait.operation));
        if (!registration) { const auto result = error(registration.status()); cancel_child_wait(kernel, object); return result; }
        wait.ticket = registration.value();
        const auto origin = syscall_origins.identity(process, *thread);
        if (origin.capture && !syscall_origins.rearm(origin, thread->native_frame, before, child_origin_binding(object, wait)))
            syscall_origins.revoke(*thread);
        emit_child_wait(wait, "rearm"); complete = false; return -11;
    }
    const auto pid = wait.pid;
    const auto request = wait.operation;
    cancel_child_wait(kernel, object, "complete", ready);
    return complete_child_wait(kernel, pid, request, ready);
}
namespace
{
i64 start_child_wait(Kernel &kernel, sched::ProcessId pid, const ChildWaitRequest &request, bool &complete)
{
    auto *process = kernel.scheduler().find(pid);
    auto *thread = kernel.scheduler().current_thread();
    if (!process || !thread || thread->owner != pid || thread->state != sched::ProcessState::running) return -22;
    if (const auto previous = child_wait_object(pid, thread->tid); previous.pool)
        return resume_child_wait(kernel, *process, previous, complete);
    const auto ready = child_wait_probe(kernel, pid, request);
    if (ready != -11) return complete_child_wait(kernel, pid, request, ready);
    PollWaitIndex *slot = nullptr;
    for (auto &entry : child_waits) if (!entry.pid) { slot = &entry; break; }
    if (!slot) return -12;
#ifndef OK_KERNEL_FREESTANDING
    if (auto status = activate_pool(); !status) return error(status);
#endif
    const auto allocated = child_wait_objects.create();
    if (!allocated) return error(allocated.status());
    const auto object = allocated.value();
    auto resolved = child_wait_objects.resolve(object);
    if (!resolved) { runtime_failure = resolved.status(); return error(resolved.status()); }
    auto &wait = *resolved.value();
    wait.pid = pid; wait.tid = thread->tid; wait.operation = request;
    const auto ticket = kernel.scheduler().register_current_wait(sched::ThreadWaitReason::child, child_wait_key(request));
    if (!ticket) { cancel_child_wait(kernel, object); return error(ticket.status()); }
    wait.ticket = ticket.value(); wait.request = wait.ticket.generation;
    *slot = {pid, wait.tid, object};
    const auto origin = syscall_origins.identity(*process, *thread);
    if (syscall_origins.live(origin) && !syscall_origins.bind(origin, thread->native_frame, child_origin_binding(object, wait)))
        syscall_origins.revoke(*thread);
    emit_child_wait(wait, "arm"); complete = false; return -11;
}
} // namespace
} // namespace runtime_detail

void finish_poll_timeout(const hw::AddressSpace &space, const PollTimeoutOutput &output, u64 now)
{
    if (!output.address || (output.field_bytes != 4 && output.field_bytes != 8))
        return;
    constexpr i64 second = 1000000000;
    i64 seconds = output.end.seconds - static_cast<i64>(now / second);
    i64 nanos = output.end.nanoseconds - static_cast<i64>(now % second);
    if (nanos < 0) { --seconds; nanos += second; }
    if (seconds < 0) seconds = nanos = 0;
    std::array<u8, 16> bytes{};
    for (usize field = 0; field < 2; ++field)
    {
        const u64 value = static_cast<u64>(field ? (output.format == PollTimeoutFormat::timeval ? nanos / 1000 : nanos) : seconds);
        for (usize i = 0; i < output.field_bytes; ++i)
            bytes[field * output.field_bytes + i] = static_cast<u8>(value >>
                (8 * (output.big_endian ? output.field_bytes - 1 - i : i)));
    }
    // Linux poll_select_finish ignores timeout copyout failure when there is no
    // signal restart. It must never replace the descriptor operation's result.
    static_cast<void>(hw::copy_to_user(space, output.address, bytes.data(), output.field_bytes * 2));
}

i64 begin_poll_signal_mask(Kernel &kernel, sched::ProcessId pid, sched::SignalMask requested)
{
#ifdef OK_PRIVATE_POLL_MASK
    auto *process = kernel.scheduler().find(pid);
    auto *thread = kernel.scheduler().current_thread();
    if (!process || !thread || thread->owner != pid || kernel.scheduler().current_pid() != pid ||
        thread->state != sched::ProcessState::running || !thread->native_active ||
        process->resources().personality != Personality::linux || !validate_entry_stack(entry_stacks, *thread)) return -22;
    if (signal_waits.find(*thread)) return -22;
    return poll_signal_masks.begin(*thread, requested) ? 0 : -22;
#else
    static_cast<void>(kernel); static_cast<void>(pid); static_cast<void>(requested);
    return -38;
#endif
}
void finish_poll_signal_mask(Kernel &kernel, sched::ProcessId pid, bool complete)
{
#ifdef OK_PRIVATE_POLL_MASK
    auto *thread = kernel.scheduler().current_thread();
    if (complete && thread && thread->owner == pid) poll_signal_masks.restore(*thread);
#else
    static_cast<void>(kernel); static_cast<void>(pid); static_cast<void>(complete);
#endif
}

static i64 begin_readiness_wait(Kernel &kernel, sched::ProcessId pid, const PollRequest &request,
                                bool &complete, const SelectSets *sets = nullptr)
{
    auto *process = kernel.scheduler().find(pid);
    auto *current = kernel.scheduler().current_thread();
    if (!process || !current || current->owner != pid || current->state != sched::ProcessState::running ||
        kernel.scheduler().current_pid() != pid)
        return -22;
    auto &space = process->resources().native_space;
    auto finish = [&](i64 result) {
        if (request.timeout_output.address)
            if (auto now = arch::monotonic_nanoseconds(); now)
                finish_poll_timeout(space, request.timeout_output, now.value());
        return result;
    };
    if (request.descriptors.size() > sched::max_process_fds)
        return finish(-22);
    for (const auto &candidate : poll_waits)
        if (candidate.pid == pid && candidate.tid == current->tid) return finish(-22);
    PollWaitIndex *entry = nullptr;
    for (auto &candidate : poll_waits)
        if (candidate.pid == 0) { entry = &candidate; break; }
    if (!entry) return finish(-12);
#ifndef OK_KERNEL_FREESTANDING
    if (auto status = activate_pool(); !status) return finish(error(status));
#endif
    auto allocated = poll_objects.create();
    if (!allocated) return finish(error(allocated.status()));
    const auto object = allocated.value();
    auto resolved = poll_objects.resolve(object);
    if (!resolved)
    {
        if (auto status = poll_objects.destroy(object); !status) runtime_failure = status;
        if (auto status = poll_objects.trim_empty_pages(); !status) runtime_failure = status;
        return finish(error(resolved.status()));
    }
    auto *wait = resolved.value();
    wait->pid = pid;
    wait->tid = current->tid;
    wait->console_epoch = process->console_epoch();
    wait->output = request.output;
    wait->count = request.descriptors.size();
    wait->descriptions.fill(sched::invalid_description);
    wait->timeout_output = request.timeout_output;
    wait->big_endian = request.big_endian;
    wait->read_normal = request.read_normal;
    wait->write_normal = request.write_normal;
    wait->linux_events = request.linux_events;
    wait->select_sets = sets != nullptr;
    if (sets) wait->sets = *sets;
    auto done = [&](i64 result) { cancel_poll(kernel, object); return finish(result); };
    for (usize i = 0; i < wait->count; ++i)
    {
        wait->fds[i] = request.descriptors[i];
        const auto *slot = process->fd_table().entry(wait->fds[i].fd);
        if (slot && slot->used)
        {
            if (auto status = kernel.scheduler().retain_description(slot->description); !status)
                return done(error(status));
            wait->descriptions[i] = slot->description;
        }
    }
    auto ready = poll_ready(kernel, *wait);
    if (!ready) return done(error(ready.status()));
    if (ready.value() || request.immediate)
        return done(poll_output(space, *wait) ? static_cast<i64>(ready.value()) : -14);
    if (request.timed)
    {
        auto now = arch::monotonic_nanoseconds();
        if (!now) return done(error(now.status()));
        auto armed = wait->timeout.arm(now.value(), request.timeout.seconds,
                                      request.timeout.nanoseconds, request.absolute);
        if (!armed) return done(error(armed.status()));
        if (wait->timeout.complete(now.value(), armed.value()))
            return done(poll_output(space, *wait) ? 0 : -14);
    }
    auto registration = kernel.scheduler().register_current_wait(sched::ThreadWaitReason::poll, 0);
    if (!registration)
        return done(error(registration.status()));
    wait->ticket = registration.value();
#ifdef OK_PRIVATE_POLL_MASK
    if (!poll_signal_masks.bind(*current, object, wait->ticket)) return done(-22);
#endif
    *entry = {pid, current->tid, object};
    if (process->resources().personality == Personality::linux && (wait->select_sets || wait->linux_events))
    {
        wait->syscall = poll_syscall_number(current->native_frame, wait->select_sets);
        wait->resume = current->native_frame.pc;
    }
    emit_poll_arm(*wait);
    complete = false;
    return 0;
}

i64 begin_child_wait(Kernel &kernel, sched::ProcessId pid, const ChildWaitRequest &request, bool &complete)
{ return start_child_wait(kernel, pid, request, complete); }

i64 begin_poll(Kernel &kernel, sched::ProcessId pid, const PollRequest &request, bool &complete)
{
    return begin_readiness_wait(kernel, pid, request, complete);
}
i64 begin_select(Kernel &kernel, sched::ProcessId pid, const SelectRequest &request, bool &complete)
{
    static_assert(sched::max_process_fds == 32, "Select mask must follow descriptor table growth");
    auto *process = kernel.scheduler().find(pid);
    if (!process || process->threads().empty() || kernel.scheduler().current_pid() != pid ||
        request.sets.count > sched::max_process_fds ||
        (request.sets.word_bytes != 4 && request.sets.word_bytes != 8)) return -22;
    const auto allowed = request.sets.count == 32 ? ~u32{0} : request.sets.count ? (u32{1} << request.sets.count) - 1 : 0;
    for (auto mask : request.sets.interests) if (mask & ~allowed) return -22;
    auto finish = [&](i64 result) {
        if (request.readiness.timeout_output.address)
            if (auto now = arch::monotonic_nanoseconds(); now)
                finish_poll_timeout(process->resources().native_space, request.readiness.timeout_output, now.value());
        return result;
    };
    std::array<ok_pollfd, sched::max_process_fds> descriptors{};
    usize count = 0;
    const auto selected = request.sets.interests[0] | request.sets.interests[1] | request.sets.interests[2];
    for (usize fd = 0; fd < request.sets.count; ++fd)
    {
        const auto bit = u32{1} << fd;
        if (!(selected & bit)) continue;
        const auto *slot = process->fd_table().entry(static_cast<i32>(fd));
        if (!slot || !slot->used) return finish(-9);
        descriptors[count++] = {static_cast<i32>(fd), static_cast<i16>(
            (request.sets.interests[0] & bit ? OK_POLLIN : 0) |
            (request.sets.interests[1] & bit ? OK_POLLOUT : 0)), 0};
    }
    auto readiness = request.readiness;
    readiness.descriptors = {descriptors.data(), count};
    readiness.linux_events = true;
    readiness.read_normal = readiness.write_normal = 0;
    return begin_readiness_wait(kernel, pid, readiness, complete, &request.sets);
}
} // namespace ok::user::native
