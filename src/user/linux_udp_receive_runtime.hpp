// Private captured receive ownership, integrated by the serialized runtime.
[[maybe_unused]] Status linux_udp_receive_invariant(Status status)
{
    if (runtime_failure) runtime_failure = status;
    started = false;
    return status;
}
[[maybe_unused]] memory::ObjectHandle linux_udp_receive_object(sched::ProcessId pid, sched::ThreadId tid)
{
    for (const auto &entry : linux_udp_receive_waits)
        if (entry.pid == pid && entry.tid == tid) return entry.object;
    return {};
}
LinuxUdpReceiveStore::Member *linux_udp_receive_member(memory::ObjectHandle object)
{
    for (auto &entry : linux_udp_receive_waits)
        if (entry.pid && entry.object == object) return &entry;
    return nullptr;
}
SyscallOrigins::LinuxUdpReceiveBinding linux_udp_receive_binding(memory::ObjectHandle object,
                                                               const LinuxUdpReceiveWait &wait)
{
    return {object, wait.request_sequence, wait.ticket, wait.retained_description, wait.endpoint, wait.operation};
}

// Fresh current-image borrowing; generation-bearing membership is revalidated
// before and after every user-copy callback. The table borrow destructor never
// resolves an object or accesses a description/frame/PCB that could be retired.
class LinuxUdpReceiveAccess final
{
  public:
    LinuxUdpReceiveAccess(const LinuxUdpReceiveAccess &) = delete;
    LinuxUdpReceiveAccess &operator=(const LinuxUdpReceiveAccess &) = delete;
    LinuxUdpReceiveAccess &operator=(LinuxUdpReceiveAccess &&) = delete;
    LinuxUdpReceiveAccess(LinuxUdpReceiveAccess &&other) noexcept
        : kernel_(std::exchange(other.kernel_, nullptr)), object_(other.object_), binding_(other.binding_),
          borrow_(std::move(other.borrow_)) {}
    [[nodiscard]] static Result<LinuxUdpReceiveAccess> acquire(Kernel &kernel, memory::ObjectHandle object)
    {
        const auto *member = linux_udp_receive_member(object);
        const auto resolved = member ? linux_udp_receive_objects.resolve(object)
            : Result<LinuxUdpReceiveWait *>{Status::denied("Linux UDP receive access lost membership")};
        if (!resolved) return resolved.status();
        const auto &wait = *resolved.value();
        if (!runtime_failure || wait.pid != member->pid || wait.tid != member->tid || !wait.capture || !wait.binding ||
            (wait.phase != LinuxUdpReceivePhase::captured_staged && wait.phase != LinuxUdpReceivePhase::published) ||
            *wait.binding != linux_udp_receive_binding(object, wait))
            return Status::denied("Linux UDP receive access has no exact staged/ready owner");
        auto borrow = syscall_origins.acquire_linux_udp_receive_access(*wait.capture, *wait.binding);
        if (!borrow) return borrow.status();
        return LinuxUdpReceiveAccess(kernel, object, *wait.binding, std::move(borrow.value()));
    }
    [[nodiscard]] bool live() const
    {
        if (!kernel_ || !runtime_failure) return false;
        const auto *member = linux_udp_receive_member(object_);
        const auto resolved = member ? linux_udp_receive_objects.resolve(object_)
            : Result<LinuxUdpReceiveWait *>{Status::denied("Linux UDP receive borrow membership expired")};
        if (!resolved) return false;
        const auto &wait = *resolved.value();
        return wait.pid == member->pid && wait.tid == member->tid && wait.capture && wait.binding &&
            *wait.binding == binding_ && linux_udp_receive_binding(object_, wait) == binding_ &&
            (wait.phase == LinuxUdpReceivePhase::captured_staged || wait.phase == LinuxUdpReceivePhase::published) &&
            borrow_.live();
    }
    [[nodiscard]] Status range(u64 address, usize count)
    {
        if (!live()) return lost();
        constexpr u64 mask = lu::Codec<lu::configured_native_profile()>::word_mask;
        if (address > mask || count > mask || address > hw::user_end || count > hw::user_end - address ||
            (count && address < hw::user_begin)) return Status::fault("Linux UDP receive borrow interval wraps");
        return {};
    }
    [[nodiscard]] Status read(u64 address, std::span<std::byte> output)
    {
        if (auto status = range(address, output.size()); !status) return status;
        if (output.empty()) return {};
        auto space = borrow_.address_space();
        if (!space) return lost();
        const auto status = hw::copy_from_user(space.value(), output.data(), address, output.size());
        return live() ? status : lost();
    }
    [[nodiscard]] Status write(u64 address, std::span<const std::byte> input)
    {
        if (auto status = range(address, input.size()); !status) return status;
        if (input.empty()) return {};
        auto space = borrow_.address_space();
        if (!space) return lost();
        const auto status = hw::copy_to_user(space.value(), address, input.data(), input.size());
        return live() ? status : lost();
    }
  private:
    LinuxUdpReceiveAccess(Kernel &kernel, memory::ObjectHandle object, SyscallOrigins::LinuxUdpReceiveBinding binding,
        SyscallOrigins::LinuxUdpReceiveBorrow borrow) noexcept
        : kernel_(&kernel), object_(object), binding_(binding), borrow_(std::move(borrow)) {}
    static Status lost() { return Status::denied("Linux UDP receive current-image borrow expired"); }
    Kernel *kernel_{};
    memory::ObjectHandle object_{};
    SyscallOrigins::LinuxUdpReceiveBinding binding_{};
    SyscallOrigins::LinuxUdpReceiveBorrow borrow_;
};

[[maybe_unused]] Status retire_linux_udp_receive(Kernel &kernel, memory::ObjectHandle object,
                                               LinuxUdpReceiveRetirement cause)
{
    static_cast<void>(cause); // Retirement never publishes to a departed image or delivers a caught signal.
    if (!object.pool) return {};
    if (object.pool != &linux_udp_receive_objects)
        return linux_udp_receive_invariant(Status::fault("foreign Linux UDP receive object"));
    auto *member = linux_udp_receive_member(object);
    if (!member) return {}; // Never resolve a retired/recycled generation.
    auto resolved = linux_udp_receive_objects.resolve(object);
    if (!resolved) return linux_udp_receive_invariant(resolved.status());
    auto &owned = *resolved.value();
    if (owned.pid != member->pid || owned.tid != member->tid || owned.phase == LinuxUdpReceivePhase::taken)
        return linux_udp_receive_invariant(Status::fault("Linux UDP receive membership changed"));
    if (owned.capture && syscall_origins.linux_udp_receive_borrowed(*owned.capture))
        return linux_udp_receive_invariant(Status::busy("Linux UDP receive retirement must end current borrow first"));
    LinuxUdpReceiveWait detached{std::move(owned)};
    owned.phase = LinuxUdpReceivePhase::taken;
    owned.capture.reset(); owned.binding.reset(); owned.ticket = {};
    owned.retained_description = sched::invalid_description;
    *member = {}; // Consume every release edge before the first cleanup boundary.
    Status failure{};
    auto record = [&](Status status) { if (!status && failure) failure = status; };
    if (detached.capture && detached.binding)
        record(syscall_origins.retire_linux_udp_receive(*detached.capture, *detached.binding));
    if (detached.ticket.generation)
    {
        const auto *thread = kernel.scheduler().find_thread(detached.pid, detached.tid);
        if (thread && thread->wait_registration.active())
        {
            if (thread->wait_registration.ticket() != detached.ticket ||
                !kernel.scheduler().complete_wait(detached.ticket, sched::WaitCompletion::cancelled))
                record(Status::fault("Linux UDP receive retirement lost the exact owned ticket"));
        }
    }
    record(linux_udp_receive_objects.destroy(object));
    record(linux_udp_receive_objects.trim_empty_pages());
    if (detached.retained_description != sched::invalid_description)
        record(kernel.scheduler().release_description(detached.retained_description));
    return failure ? Status{} : linux_udp_receive_invariant(failure);
}
[[maybe_unused]] Status retire_linux_udp_receives(Kernel &kernel, sched::ProcessId pid,
    std::optional<sched::ThreadId> tid, LinuxUdpReceiveRetirement cause)
{
    Status failure{};
    for (auto &entry : linux_udp_receive_waits)
        if (entry.pid == pid && (!tid || entry.tid == *tid))
        {
            if (!entry.object.pool) { entry = {}; continue; } // Reserved before object allocation.
            if (auto status = retire_linux_udp_receive(kernel, entry.object, cause); !status && failure) failure = status;
        }
    return failure;
}
[[maybe_unused]] Status retire_linux_udp_receives_for_exec(Kernel &kernel, sched::ProcessId pid)
{ return retire_linux_udp_receives(kernel, pid, {}, LinuxUdpReceiveRetirement::exec); }

Status linux_udp_receive_start_status()
{
    for (const auto &entry : linux_udp_receive_waits)
        if (entry.pid || entry.tid || entry.object.pool)
            return Status::busy("native start must not discard existing Linux UDP receive membership");
    if (linux_udp_receive_objects.accounting().live_objects)
        return Status::busy("native start must not discard existing Linux UDP receive objects");
    return {};
}

// Orphan erasure must follow checked receive retirement while the canonical PCB
// still exists. Missing owners indicate a violated lifecycle boundary; preserve
// their evidence and contain instead of treating disappearance as completion.
Status prune_departed_linux_udp_receives(Kernel &kernel)
{
    Status failure{};
    for (const auto &entry : linux_udp_receive_waits)
    {
        if (!entry.pid) continue;
        const auto *process = kernel.scheduler().find(entry.pid);
        const auto *thread = kernel.scheduler().find_thread(entry.pid, entry.tid);
        if (!process || !thread)
            return linux_udp_receive_invariant(Status::fault("Linux UDP receive owner disappeared before checked retirement"));
        if (process->state() == sched::ProcessState::zombie || process->state() == sched::ProcessState::exited ||
            thread->state == sched::ProcessState::exited)
        {
            if (!entry.object.pool)
                return linux_udp_receive_invariant(Status::fault("departed Linux UDP receive has an allocation reservation"));
            if (auto status = retire_linux_udp_receive(kernel, entry.object, LinuxUdpReceiveRetirement::process_exit);
                !status && failure) failure = status;
        }
    }
    return failure;
}

linux_udp::ReceiveScratch linux_udp_owned_receive_scratch;
Result<usize> copy_linux_udp_received(Kernel &kernel, memory::ObjectHandle object)
{
    auto admitted = LinuxUdpReceiveAccess::acquire(kernel, object);
    if (!admitted) return admitted.status();
    auto &access = admitted.value();
    const auto resolved = linux_udp_receive_objects.resolve(object);
    if (!resolved) return resolved.status();
    const auto operation = resolved.value()->operation;
    const auto endpoint = resolved.value()->endpoint;
    const auto received = linux_udp_owned_receive_scratch.receive<lu::configured_native_profile()>(
        kernel.native_udp(), endpoint, access, {.payload=operation.payload, .count=operation.count,
        .source=operation.address, .source_length=operation.source_length, .flags=operation.flags});
    return access.live() ? received : Result<usize>{Status::denied("Linux UDP receive lost borrowed completion")};
} // The access destructor runs here, before any result/origin/object/OFD retirement.

Status complete_linux_udp_receive(Kernel &kernel, memory::ObjectHandle object, i64 target_result)
{
    auto *member = linux_udp_receive_member(object);
    auto resolved = member ? linux_udp_receive_objects.resolve(object)
        : Result<LinuxUdpReceiveWait *>{Status::denied("Linux UDP receive completion lost membership")};
    if (!resolved) return linux_udp_receive_invariant(resolved.status());
    auto &wait = *resolved.value();
    auto *process = kernel.scheduler().find(wait.pid);
    auto *thread = kernel.scheduler().find_thread(wait.pid, wait.tid);
    Status published{};
    if (!runtime_failure || !process || !thread || member->pid != wait.pid || member->tid != wait.tid ||
        !wait.capture || !wait.binding || *wait.binding != linux_udp_receive_binding(object, wait) ||
        (wait.phase != LinuxUdpReceivePhase::captured_staged && wait.phase != LinuxUdpReceivePhase::published))
        published = Status::denied("Linux UDP receive completion lost owner");
    else
        published = syscall_origins.consume_linux_udp_receive(*wait.capture, *wait.binding);
    if (published)
    {
        thread->syscall_pending = false;
        published = set_linux_udp_result(*process, thread->native_frame, target_result);
    }
    // Failure never grants the old synchronous publisher or a retry. Contain
    // before release callbacks, then retire once even if the encoder/gate failed.
    if (!published) static_cast<void>(linux_udp_receive_invariant(published));
    const auto retired = retire_linux_udp_receive(kernel, object, LinuxUdpReceiveRetirement::completed);
    return !published ? published : retired;
}
Status linux_udp_receive_post_arm(Kernel &kernel, const LinuxUdpReceiveWait &wait)
{
    const auto ready = kernel.native_udp_executor().readiness(kernel.native_udp(), wait.endpoint);
    const auto *description = kernel.scheduler().open_description(wait.retained_description);
    if (!ready || !description || (description->flags & posix::o_NONBLOCK) ||
        !ready.value().may_wait(wait.observed_receive, wait.observed_executor))
        if (!kernel.scheduler().complete_wait(wait.ticket, sched::WaitCompletion::ready))
            return linux_udp_receive_invariant(Status::fault("Linux UDP receive post-arm lost exact ticket"));
    return {};
}
[[maybe_unused]] Result<LinuxUdpReceiveBegin> begin_linux_udp_receive(Kernel &kernel,
    sched::ProcessControlBlock &process, sched::ThreadControlBlock &thread,
    LinuxUdpImmediateAccess<lu::configured_native_profile()> &access,
    linux_udp::OriginalUdpDescription &original, LinuxUdpReceiveAdmissionToken &&admission)
{
    constexpr auto profile = lu::configured_native_profile();
    auto immediate = [](Status status) { return LinuxUdpReceiveBegin{LinuxUdpReceiveBeginKind::immediate,
                                                                   linux_udp_receive_status<profile>(status)}; };
    if (kernel.scheduler().find(process.pid()) != &process ||
        kernel.scheduler().find_thread(process.pid(), thread.tid) != &thread ||
        kernel.scheduler().current_thread() != &thread || !runtime_failure)
        return linux_udp_receive_invariant(Status::denied("Linux UDP receive begin has no canonical live owner"));
    if (auto status = linux_udp_receive_store.validate(kernel, process, thread, access, admission, original); !status)
        return linux_udp_receive_invariant(status);
    for (const auto &entry : linux_udp_receive_waits)
        if (entry.pid == process.pid() && entry.tid == thread.tid)
            return linux_udp_receive_invariant(Status::fault("duplicate Linux UDP receive ownership"));
    bool available = false;
    for (const auto &entry : linux_udp_receive_waits) if (!entry.pid) { available = true; break; }
    if (!available) return immediate(Status::no_memory("Linux UDP receive membership pressure"));
#ifndef OK_KERNEL_FREESTANDING
    if (auto status = activate_pool(); !status) return immediate(status);
#endif
    const auto staged = linux_udp_receive_store.stage(kernel, process, thread, access, original, admission);
    if (!staged.ok())
    {
        if (staged.contained || (staged.status.code() != StatusCode::no_memory &&
            staged.status.code() != StatusCode::busy && staged.status.code() != StatusCode::overflow))
            return linux_udp_receive_invariant(staged.status);
        return immediate(staged.status);
    }
    const auto object = staged.object;
    auto resolved = linux_udp_receive_objects.resolve(object);
    if (!resolved) return linux_udp_receive_invariant(resolved.status());
    auto &wait = *resolved.value();
    auto fail_owned = [&](Status status) -> Result<LinuxUdpReceiveBegin> {
        static_cast<void>(linux_udp_receive_invariant(status));
        static_cast<void>(retire_linux_udp_receive(kernel, object, LinuxUdpReceiveRetirement::rollback));
        return status;
    };
    auto finish = [&](i64 result) -> Result<LinuxUdpReceiveBegin> {
        if (auto status = complete_linux_udp_receive(kernel, object, result); !status) return status;
        return LinuxUdpReceiveBegin{LinuxUdpReceiveBeginKind::owned_completed, {}};
    };
    const auto received = copy_linux_udp_received(kernel, object);
    if (!received && received.status().code() == StatusCode::denied) return fail_owned(received.status());
    if (received || received.status().code() != StatusCode::would_block)
        return finish(received ? static_cast<i64>(received.value()) : linux_udp_receive_status<profile>(received.status()));
    const auto *description = kernel.scheduler().open_description(wait.retained_description);
    if (!description) return fail_owned(Status::fault("Linux UDP receive staged original description disappeared"));
    if (!kernel.native_udp_executor().status()) return finish(linux_udp_receive_errno<profile>(lu::UdpErrno::e_io));
    if (description->flags & posix::o_NONBLOCK) return finish(linux_udp_receive_errno<profile>(lu::UdpErrno::e_again));
    const auto ready = kernel.native_udp_executor().readiness(kernel.native_udp(), wait.endpoint);
    if (!ready) return finish(linux_udp_receive_status<profile>(ready.status()));
    wait.observed_receive = ready.value().receive.sequence;
    wait.observed_executor = ready.value().executor.readiness_sequence;
    if (!ready.value().may_wait(wait.observed_receive, wait.observed_executor))
        return finish(linux_udp_receive_errno<profile>(lu::UdpErrno::e_io));
    const auto ticket = kernel.scheduler().register_current_wait(sched::ThreadWaitReason::udp_receive, wait.endpoint);
    if (!ticket) return finish(linux_udp_receive_status<profile>(ticket.status()));
    wait.ticket = ticket.value(); // Discoverable immediately; retirement owns this exact ticket.
    wait.request_sequence = wait.ticket.generation;
    const auto bound = linux_udp_receive_binding(object, wait);
    if (auto status = syscall_origins.bind_linux_udp_receive(*wait.capture, thread.native_frame, bound); !status)
        return fail_owned(status);
    wait.binding = bound;
    wait.phase = LinuxUdpReceivePhase::published;
    if (auto status = linux_udp_receive_post_arm(kernel, wait); !status) return fail_owned(status);
    return LinuxUdpReceiveBegin{LinuxUdpReceiveBeginKind::deferred, {}};
}
[[maybe_unused]] Status resume_linux_udp_receive(Kernel &kernel, sched::ProcessControlBlock &process,
    sched::ThreadControlBlock &thread, memory::ObjectHandle object)
{
    constexpr auto profile = lu::configured_native_profile();
    auto *member = linux_udp_receive_member(object);
    auto resolved = member ? linux_udp_receive_objects.resolve(object)
        : Result<LinuxUdpReceiveWait *>{Status::fault("Linux UDP receive resume lost membership")};
    if (!resolved) return linux_udp_receive_invariant(resolved.status());
    auto &wait = *resolved.value();
    auto fail = [&](Status status) {
        static_cast<void>(linux_udp_receive_invariant(status));
        static_cast<void>(retire_linux_udp_receive(kernel, object, LinuxUdpReceiveRetirement::rollback));
        return status;
    };
    if (process.pid() != wait.pid || thread.owner != wait.pid || thread.tid != wait.tid ||
        member->pid != wait.pid || member->tid != wait.tid || wait.phase != LinuxUdpReceivePhase::published ||
        !wait.capture || !wait.binding || *wait.binding != linux_udp_receive_binding(object, wait) ||
        kernel.scheduler().find(process.pid()) != &process || kernel.scheduler().find_thread(wait.pid, wait.tid) != &thread ||
        kernel.scheduler().current_thread() != &thread || thread.wait_registration.active() ||
        !syscall_origins.validate_linux_udp_receive(*wait.capture, thread.native_frame, *wait.binding, sched::WaitCompletion::ready))
        return fail(Status::denied("Linux UDP receive resume is outside the exact ready owner"));
    const auto received = copy_linux_udp_received(kernel, object); // Ends the borrow before all branches below.
    if (!received && received.status().code() == StatusCode::denied) return fail(received.status());
    if (received || received.status().code() != StatusCode::would_block)
        return complete_linux_udp_receive(kernel, object,
            received ? static_cast<i64>(received.value()) : linux_udp_receive_status<profile>(received.status()));
    const auto *description = kernel.scheduler().open_description(wait.retained_description);
    if (!description) return fail(Status::fault("Linux UDP receive original description disappeared"));
    if (!kernel.native_udp_executor().status())
        return complete_linux_udp_receive(kernel, object, linux_udp_receive_errno<profile>(lu::UdpErrno::e_io));
    if (description->flags & posix::o_NONBLOCK)
        return complete_linux_udp_receive(kernel, object, linux_udp_receive_errno<profile>(lu::UdpErrno::e_again));
    const auto ready = kernel.native_udp_executor().readiness(kernel.native_udp(), wait.endpoint);
    if (!ready || !ready.value().may_wait(ready.value().receive.sequence, ready.value().executor.readiness_sequence))
        return complete_linux_udp_receive(kernel, object, linux_udp_receive_errno<profile>(lu::UdpErrno::e_io));
    wait.observed_receive = ready.value().receive.sequence;
    wait.observed_executor = ready.value().executor.readiness_sequence;
    const auto ticket = kernel.scheduler().register_current_wait(sched::ThreadWaitReason::udp_receive, wait.endpoint);
    if (!ticket) return complete_linux_udp_receive(kernel, object, linux_udp_receive_status<profile>(ticket.status()));
    wait.ticket = ticket.value(); // A failed origin rearm still owns the newer scheduler ticket.
    auto after = *wait.binding;
    after.ticket = wait.ticket;
    if (auto status = syscall_origins.rearm_linux_udp_receive(*wait.capture, thread.native_frame, *wait.binding, after); !status)
        return fail(status);
    wait.binding = after;
    if (auto status = linux_udp_receive_post_arm(kernel, wait); !status) return fail(status);
    return {};
}
[[maybe_unused]] Status scan_linux_udp_receives(Kernel &kernel)
{
    for (const auto &entry : linux_udp_receive_waits)
    {
        if (!entry.pid || !entry.object.pool) continue; // Discoverable allocation reservation.
        const auto object = entry.object;
        auto resolved = linux_udp_receive_objects.resolve(object);
        if (!resolved) return linux_udp_receive_invariant(resolved.status());
        auto &wait = *resolved.value();
        auto fail = [&](Status status) {
            static_cast<void>(linux_udp_receive_invariant(status));
            static_cast<void>(retire_linux_udp_receive(kernel, object, LinuxUdpReceiveRetirement::rollback));
            return status;
        };
        if (wait.pid != entry.pid || wait.tid != entry.tid || wait.phase == LinuxUdpReceivePhase::taken)
            return fail(Status::fault("Linux UDP receive scan membership changed"));
        if (wait.phase == LinuxUdpReceivePhase::object_staged) continue;
        const auto *process = kernel.scheduler().find(wait.pid);
        const auto *thread = kernel.scheduler().find_thread(wait.pid, wait.tid);
        if (!process || !thread || process->state() == sched::ProcessState::exited ||
            process->state() == sched::ProcessState::zombie || thread->state == sched::ProcessState::exited)
        {
            if (auto status = retire_linux_udp_receive(kernel, object, LinuxUdpReceiveRetirement::process_exit); !status) return status;
            continue;
        }
        const auto cause = thread->wait_registration.active() ? sched::WaitCompletion::none : thread->wait_registration.completion();
        if (!wait.capture || !wait.binding || *wait.binding != linux_udp_receive_binding(object, wait) ||
            !syscall_origins.validate_linux_udp_receive(*wait.capture, thread->native_frame, *wait.binding,
                wait.phase == LinuxUdpReceivePhase::captured_staged ? sched::WaitCompletion::none : cause))
            return fail(Status::denied("Linux UDP receive scan lost current captured owner"));
        if (wait.phase == LinuxUdpReceivePhase::published && thread->wait_registration.active())
            if (auto status = linux_udp_receive_post_arm(kernel, wait); !status) return fail(status);
    }
    return {};
}
