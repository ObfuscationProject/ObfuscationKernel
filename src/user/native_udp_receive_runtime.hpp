// Included after the existing result mapper, inside the private runtime namespace.
Status native_udp_receive_invariant(Status status)
{
    if (runtime_failure) runtime_failure = status;
    started = false;
    return status;
}
memory::ObjectHandle native_udp_receive_object(sched::ProcessId pid, sched::ThreadId tid)
{
    for (const auto &entry : native_udp_receive_waits)
        if (entry.pid == pid && entry.tid == tid) return entry.object;
    return {};
}
PollWaitIndex *native_udp_receive_member(memory::ObjectHandle object)
{
    for (auto &entry : native_udp_receive_waits)
        if (entry.object == object && entry.pid) return &entry;
    return nullptr;
}
void retire_native_udp_receive(Kernel &kernel, memory::ObjectHandle object)
{
    if (!object.pool) return;
    if (object.pool != &native_udp_receive_objects)
    {
        static_cast<void>(native_udp_receive_invariant(Status::fault("foreign UDP receive object")));
        return;
    }
    auto *member = native_udp_receive_member(object);
    if (!member) return; // Exact membership was already taken; never resolve a reused slot.
    auto resolved = native_udp_receive_objects.resolve(object);
    if (!resolved)
    {
        static_cast<void>(native_udp_receive_invariant(resolved.status()));
        return;
    }
    auto &owned = *resolved.value();
    if (owned.pid != member->pid || owned.tid != member->tid || owned.phase == NativeUdpOwnerPhase::taken)
    {
        static_cast<void>(native_udp_receive_invariant(Status::fault("UDP receive membership changed")));
        return;
    }
    const NativeUdpReceiveWait detached = owned;
    owned.phase = NativeUdpOwnerPhase::taken;
    owned.ticket = {};
    owned.retained_description = sched::invalid_description;
    owned.origin_binding.reset();
    *member = {};
    // All release edges are consumed locally before any fallible/callback boundary.
    if (detached.origin_binding)
        syscall_origins.retire_native_udp(detached.capture, *detached.origin_binding);
    if (detached.ticket.generation)
        static_cast<void>(kernel.scheduler().complete_wait(detached.ticket, sched::WaitCompletion::cancelled));
    Status failure{};
    if (auto status = native_udp_receive_objects.destroy(object); !status) failure = status;
    if (auto status = native_udp_receive_objects.trim_empty_pages(); !status && failure) failure = status;
    if (detached.retained_description != sched::invalid_description)
        if (auto status = kernel.scheduler().release_description(detached.retained_description); !status && failure)
            failure = status;
    if (!failure) static_cast<void>(native_udp_receive_invariant(failure));
}
void retire_native_udp_receives(Kernel &kernel, sched::ProcessId pid,
                               std::optional<sched::ThreadId> tid = {})
{
    for (const auto &entry : native_udp_receive_waits)
        if (entry.pid == pid && (!tid || entry.tid == *tid))
            retire_native_udp_receive(kernel, entry.object);
}
Status retire_native_udp_receives_for_exec(Kernel &kernel, sched::ProcessId pid)
{
    // Called after scheduler commit while every old entry lease remains escrowed.
    retire_native_udp_receives(kernel, pid);
    return runtime_failure;
}
Result<usize> copy_native_udp_datagram(Kernel &kernel, const hw::AddressSpace &space,
    u32 endpoint, const SyscallOrigins::NativeUdpReceiveRequest &operation)
{
    if (operation.flags || (!!operation.source_address != !!operation.source_length) ||
        (operation.source_address ? operation.source_capacity < sizeof(ok_sockaddr_in) : operation.source_capacity != 0))
        return Status::invalid_argument("invalid copied native UDP receive request");
    const auto received = kernel.native_udp().peek(endpoint);
    if (!received)
    {
        if (received.status().code() == StatusCode::would_block)
            if (auto status = kernel.native_udp_executor().status(); !status) return status;
        return received.status();
    }
    const usize copied = operation.count < received.value().payload.size()
        ? operation.count : received.value().payload.size();
    // Preflight every effective destination before publishing any output.
    if ((copied && !hw::user_range(space, operation.output, copied, true)) ||
        (operation.source_address &&
         (!hw::user_range(space, operation.source_address, sizeof(ok_sockaddr_in), true) ||
          !hw::user_range(space, operation.source_length, sizeof(u32), true))))
        return Status::fault("native UDP receive output is not writable");
    if (copied && !hw::copy_to_user(space, operation.output, received.value().payload.data(), copied))
        return Status::fault("native UDP payload publication failed");
    if (operation.source_address)
    {
        ok_sockaddr_in source{};
        source.family = OK_AF_INET;
        source.port_be[0] = static_cast<u8>(received.value().source_port >> 8);
        source.port_be[1] = static_cast<u8>(received.value().source_port);
        for (usize at = 0; at < 4; ++at) source.address[at] = received.value().source[at];
        constexpr u32 actual = sizeof(source);
        if (!hw::copy_to_user(space, operation.source_address, &source, sizeof(source)) ||
            !hw::copy_to_user(space, operation.source_length, &actual, sizeof(actual)))
            return Status::fault("native UDP source publication failed");
    }
    if (auto status = kernel.native_udp().consume(endpoint); !status) return status;
    return copied;
}
SyscallOrigins::NativeUdpReceiveBinding native_udp_receive_binding(
    memory::ObjectHandle object, const NativeUdpReceiveWait &wait)
{
    return {object, wait.ticket, wait.request, wait.retained_description, wait.endpoint, wait.operation};
}
Status native_udp_receive_post_arm(Kernel &kernel, const NativeUdpReceiveWait &wait)
{
    const auto ready = kernel.native_udp_executor().readiness(kernel.native_udp(), wait.endpoint);
    if (!ready || !ready.value().may_wait(wait.observed_receive, wait.observed_executor))
        if (!kernel.scheduler().complete_wait(wait.ticket, sched::WaitCompletion::ready))
            return native_udp_receive_invariant(Status::fault("UDP receive post-arm ticket changed"));
    return {};
}
i64 begin_native_udp_receive(Kernel &kernel, sched::ProcessControlBlock &process,
    sched::ThreadControlBlock &thread, u16 description_id,
    const SyscallOrigins::NativeUdpReceiveRequest &operation, bool &complete)
{
    const auto *description = kernel.scheduler().open_description(description_id);
    if (!description || !description->references || !description->readable || !description->native_udp_handle)
        return -9;
    const u32 endpoint = description->native_udp_handle;
    const auto immediate = copy_native_udp_datagram(kernel, process.resources().native_space, endpoint, operation);
    if (immediate || immediate.status().code() != StatusCode::would_block || (description->flags & posix::o_NONBLOCK))
        return immediate ? static_cast<i64>(immediate.value()) : error(immediate.status());
    if (native_udp_receive_object(process.pid(), thread.tid).pool)
        return error(native_udp_receive_invariant(Status::fault("duplicate UDP receive owner")));
    const auto captured = syscall_origins.native_udp_capture(process, thread);
    if (!captured) return error(captured.status());
    PollWaitIndex *slot = nullptr;
    for (auto &entry : native_udp_receive_waits)
        if (!entry.pid) { slot = &entry; break; }
    if (!slot) return -12;
#ifndef OK_KERNEL_FREESTANDING
    if (auto status = activate_pool(); !status) return error(status);
#endif
    *slot = {process.pid(), thread.tid, {}};
    const auto created = native_udp_receive_objects.create(process.pid(), thread.tid, captured.value(), operation, endpoint);
    if (!created) { *slot = {}; return error(created.status()); }
    const auto object = created.value();
    slot->object = object; // Staged ownership is discoverable before any fallible step.
    auto resolved = native_udp_receive_objects.resolve(object);
    if (!resolved) return error(native_udp_receive_invariant(resolved.status()));
    auto &wait = *resolved.value();
    auto fail = [&](Status status) {
        retire_native_udp_receive(kernel, object);
        return error(status);
    };
    if (auto status = kernel.scheduler().retain_description(description_id); !status) return fail(status);
    wait.retained_description = description_id;
    description = kernel.scheduler().open_description(description_id);
    if (!description || !description->readable || !description->references || description->native_udp_handle != endpoint)
        return fail(Status::bad_descriptor("retained UDP receive endpoint changed"));
    const auto snapshot = kernel.native_udp_executor().readiness(kernel.native_udp(), endpoint);
    if (!snapshot) return fail(snapshot.status());
    wait.observed_receive = snapshot.value().receive.sequence;
    wait.observed_executor = snapshot.value().executor.readiness_sequence;
    if (!snapshot.value().may_wait(wait.observed_receive, wait.observed_executor))
    {
        retire_native_udp_receive(kernel, object);
        if (!runtime_failure) return error(runtime_failure);
        const auto received = copy_native_udp_datagram(kernel, process.resources().native_space, endpoint, operation);
        return received ? static_cast<i64>(received.value()) : error(received.status());
    }
    const auto registration = kernel.scheduler().register_current_wait(sched::ThreadWaitReason::udp_receive, endpoint);
    if (!registration) return fail(registration.status());
    wait.ticket = registration.value();
    wait.request = wait.ticket.generation;
    const auto binding = native_udp_receive_binding(object, wait);
    if (auto status = syscall_origins.bind_native_udp(wait.capture, thread.native_frame, binding); !status) return fail(status);
    wait.origin_binding = binding;
    wait.phase = NativeUdpOwnerPhase::published;
    if (auto status = native_udp_receive_post_arm(kernel, wait); !status) return fail(status);
    complete = false;
    return -11;
}
i64 resume_native_udp_receive(Kernel &kernel, sched::ProcessControlBlock &process,
                             memory::ObjectHandle object, bool &complete)
{
    auto *member = native_udp_receive_member(object);
    auto resolved = member ? native_udp_receive_objects.resolve(object)
                           : Result<NativeUdpReceiveWait *>{Status::fault("UDP receive resume lost membership")};
    if (!resolved) return error(native_udp_receive_invariant(resolved.status()));
    auto &wait = *resolved.value();
    auto finish_result = [&](i64 result) { retire_native_udp_receive(kernel, object); return result; };
    auto *thread = kernel.scheduler().find_thread(wait.pid, wait.tid);
    if (process.pid() != wait.pid || member->pid != wait.pid || member->tid != wait.tid ||
        wait.phase != NativeUdpOwnerPhase::published || !wait.origin_binding || !thread ||
        kernel.scheduler().current_thread() != thread || thread->wait_registration.ticket() != wait.ticket ||
        thread->wait_registration.active()) return finish_result(-125);
    using State = SyscallOrigins::NativeUdpWaitState;
    const auto cause = thread->wait_registration.completion();
    const auto state = cause == sched::WaitCompletion::ready ? std::optional<State>{State::ready}
        : cause == sched::WaitCompletion::cancelled ? std::optional<State>{State::cancelled}
        : cause == sched::WaitCompletion::interrupted ? std::optional<State>{State::interrupted} : std::nullopt;
    if (!state || !syscall_origins.validate_native_udp(wait.capture, thread->native_frame, *wait.origin_binding, *state))
        return finish_result(-125);
    if (*state == State::cancelled) return finish_result(-125);
    if (*state == State::interrupted) return finish_result(-4);
    const auto received = copy_native_udp_datagram(kernel, process.resources().native_space, wait.endpoint, wait.operation);
    const auto *description = kernel.scheduler().open_description(wait.retained_description);
    if (received || received.status().code() != StatusCode::would_block || !description ||
        (description->flags & posix::o_NONBLOCK))
        return finish_result(received ? static_cast<i64>(received.value()) : error(received.status()));
    const auto snapshot = kernel.native_udp_executor().readiness(kernel.native_udp(), wait.endpoint);
    if (!snapshot) return finish_result(error(snapshot.status()));
    wait.observed_receive = snapshot.value().receive.sequence;
    wait.observed_executor = snapshot.value().executor.readiness_sequence;
    if (!snapshot.value().may_wait(wait.observed_receive, wait.observed_executor))
        return finish_result(-5);
    const auto registration = kernel.scheduler().register_current_wait(sched::ThreadWaitReason::udp_receive, wait.endpoint);
    if (!registration) return finish_result(error(registration.status()));
    wait.ticket = registration.value(); // Teardown owns the new ticket even if origin rearm fails.
    auto after = *wait.origin_binding;
    after.ticket = wait.ticket;
    if (auto status = syscall_origins.rearm_native_udp(wait.capture, thread->native_frame, *wait.origin_binding, after); !status)
        return finish_result(error(status));
    wait.origin_binding = after;
    if (auto status = native_udp_receive_post_arm(kernel, wait); !status) return finish_result(error(status));
    complete = false;
    return -11;
}
Status scan_native_udp_receives(Kernel &kernel)
{
    for (const auto &entry : native_udp_receive_waits)
    {
        if (!entry.pid) continue;
        const auto object = entry.object;
        auto resolved = native_udp_receive_objects.resolve(object);
        if (!resolved) return native_udp_receive_invariant(resolved.status());
        auto &wait = *resolved.value();
        if (wait.pid != entry.pid || wait.tid != entry.tid || wait.phase != NativeUdpOwnerPhase::published || !wait.origin_binding)
            return native_udp_receive_invariant(Status::fault("UDP receive scan found an unfinished owner"));
        const auto *process = kernel.scheduler().find(wait.pid);
        const auto *thread = kernel.scheduler().find_thread(wait.pid, wait.tid);
        if (!process || !thread || !thread->native_active || !thread->syscall_pending ||
            process->state() == sched::ProcessState::zombie || process->state() == sched::ProcessState::exited ||
            thread->state == sched::ProcessState::exited || thread->wait_registration.ticket() != wait.ticket)
        {
            retire_native_udp_receive(kernel, object);
            if (!runtime_failure) return runtime_failure;
            continue;
        }
        using State = SyscallOrigins::NativeUdpWaitState;
        if (!thread->wait_registration.active())
        {
            const auto cause = thread->wait_registration.completion();
            const auto state = cause == sched::WaitCompletion::ready ? std::optional<State>{State::ready}
                : cause == sched::WaitCompletion::cancelled ? std::optional<State>{State::cancelled}
                : cause == sched::WaitCompletion::interrupted ? std::optional<State>{State::interrupted} : std::nullopt;
            if (!state || !syscall_origins.validate_native_udp(wait.capture, thread->native_frame, *wait.origin_binding, *state))
            {
                retire_native_udp_receive(kernel, object);
                return native_udp_receive_invariant(Status::fault("UDP receive completed capture changed"));
            }
        }
        else if (!syscall_origins.validate_native_udp(wait.capture, thread->native_frame, *wait.origin_binding, State::pending))
        {
            retire_native_udp_receive(kernel, object);
            return native_udp_receive_invariant(Status::fault("UDP receive pending capture changed"));
        }
        else
        {
            const auto ready = kernel.native_udp_executor().readiness(kernel.native_udp(), wait.endpoint);
            const auto *description = kernel.scheduler().open_description(wait.retained_description);
            if (!ready || !description || (description->flags & posix::o_NONBLOCK) ||
                !ready.value().may_wait(wait.observed_receive, wait.observed_executor))
                if (!kernel.scheduler().complete_wait(wait.ticket, sched::WaitCompletion::ready))
                {
                    retire_native_udp_receive(kernel, object);
                    return native_udp_receive_invariant(Status::fault("UDP receive scan lost its exact ticket"));
                }
        }
        if (!runtime_failure) return runtime_failure;
    }
    return {};
}
