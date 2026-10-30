// Native and Linux UDP receive owners, TCP immediate operations and UDP socket calls.
#include "private.hpp"

namespace ok::user::native
{
inline namespace runtime_detail
{
static_assert(std::is_nothrow_copy_constructible_v<NativeUdpReceiveWait>);
static_assert(std::is_nothrow_destructible_v<NativeUdpReceiveWait>);
constinit NativeUdpReceiveObjects native_udp_receive_objects{page_pool, native_udp_receive_capacity};
std::array<PollWaitIndex, native_udp_receive_capacity> native_udp_receive_waits{};
using LinuxUdpReceiveAdmissionToken = LinuxUdpReceiveAdmission<lu::configured_native_profile()>;
using LinuxUdpReceiveTransferCoordinator = LinuxUdpReceiveCoordinator<lu::configured_native_profile()>;
static_assert(!std::is_copy_constructible_v<LinuxUdpReceiveWait>);
static_assert(std::is_nothrow_move_constructible_v<LinuxUdpReceiveWait>);
static_assert(std::is_nothrow_destructible_v<LinuxUdpReceiveWait>);
namespace
{
constexpr usize linux_udp_receive_capacity = memory::KernelEntryStackPool::capacity;
} // namespace
using LinuxUdpReceiveLayout = LinuxUdpReceiveStore::Layout;
using LinuxUdpReceiveObjects = LinuxUdpReceiveStore::Objects;
constinit LinuxUdpReceiveStore linux_udp_receive_store{syscall_origins,page_pool};
namespace
{

Status native_udp_receive_invariant(Status status)
{
    if (runtime_failure) runtime_failure = status;
    started = false;
    return status;
}
} // namespace
memory::ObjectHandle native_udp_receive_object(sched::ProcessId pid, sched::ThreadId tid)
{
    for (const auto &entry : native_udp_receive_waits)
        if (entry.pid == pid && entry.tid == tid) return entry.object;
    return {};
}
namespace
{
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
} // namespace
void retire_native_udp_receives(Kernel &kernel, sched::ProcessId pid,
                               std::optional<sched::ThreadId> tid)
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
namespace
{
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
// A receive wait made ready by a queued datagram is credited to the link.
sched::WakeSource udp_wake_source(const Result<net::NativeUdpReceiveReadiness> &ready)
{
    return ready && ready.value().receive.readable() ? interrupt::event_wake_source(interrupt::EventSource::network)
                                                     : sched::WakeSource::thread;
}
Status native_udp_receive_post_arm(Kernel &kernel, const NativeUdpReceiveWait &wait)
{
    const auto ready = kernel.native_udp_executor().readiness(kernel.native_udp(), wait.endpoint);
    if (!ready || !ready.value().may_wait(wait.observed_receive, wait.observed_executor))
        if (!kernel.scheduler().complete_wait(wait.ticket, sched::WaitCompletion::ready, udp_wake_source(ready)))
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
} // namespace
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
                if (!kernel.scheduler().complete_wait(wait.ticket, sched::WaitCompletion::ready, udp_wake_source(ready)))
                {
                    retire_native_udp_receive(kernel, object);
                    return native_udp_receive_invariant(Status::fault("UDP receive scan lost its exact ticket"));
                }
        }
        if (!runtime_failure) return runtime_failure;
    }
    return {};
}
namespace
{
linux_udp::OperationScratch linux_udp_operations;
} // namespace

// UDP errors are already target-numbered. Generic bootstrap translation would
// change MIPS ENOTSOCK (95) into EOPNOTSUPP (122).
template <lu::Profile Target>
Status encode_linux_udp_target_result(hw::UserFrame &frame, i64 result)
{
    if (result < -4095)
        return Status::invalid_argument("Linux UDP result is outside the target errno range");
    const u32 target_error = result < 0 ? static_cast<u32>(-result) : 0;
    return lu::NativeBridge<Target>::encode_return(frame,
        {target_error ? 0 : static_cast<u64>(result), target_error});
}

Status set_linux_udp_result(const sched::ProcessControlBlock &process,
                            hw::UserFrame &frame, i64 result)
{
    syscall_origins.revoke_frame(process, frame);
    if (auto status = encode_linux_udp_target_result<lu::configured_native_profile()>(frame, result); !status)
        return status;
#ifdef OK_PRIVATE_MIPS64_N64_SIGNAL
    return private_n64_completion(process, frame, result);
#elif defined(OK_PRIVATE_PPC32_SIGNAL)
    return private_ppc_completion(process, frame, result);
#else
    return Status::success();
#endif
}

bool linux_udp_frame(const hw::UserFrame &frame)
{
    return linux_udp_immediate_operation_for_frame<lu::configured_native_profile()>(frame) !=
        LinuxUdpImmediateOperation::none;
}
namespace
{

Result<LinuxUdpReceiveBegin> begin_linux_udp_receive(Kernel &kernel,
    sched::ProcessControlBlock &process, sched::ThreadControlBlock &thread,
    LinuxUdpImmediateAccess<lu::configured_native_profile()> &access,
    linux_udp::OriginalUdpDescription &original, LinuxUdpReceiveAdmissionToken &&admission);
} // namespace

// Only this concrete private authority supplies the runtime begin/publisher.
// OperationScratch exposes no arbitrary callback injection seam for handoff.
class LinuxUdpOperationRuntimeAuthority final
{
  public:
    static Status dispatch(Kernel &kernel, sched::ProcessControlBlock &process,
                           sched::ThreadControlBlock &thread)
    {
        using Access = LinuxUdpImmediateAccess<lu::configured_native_profile()>;
        auto admitted = Access::acquire(syscall_origins, process, thread);
        if (!admitted) return admitted.status();
        auto &access = admitted.value();
        auto begin = [&](Access &current, linux_udp::OriginalUdpDescription &original,
                         LinuxUdpReceiveAdmissionToken &&admission) -> Result<LinuxUdpReceiveBegin> {
            return begin_linux_udp_receive(kernel, process, thread, current, original, std::move(admission));
        };
        auto publish = [&](i64 value) -> Status {
            if (!access.live() || kernel.scheduler().find(process.pid()) != &process ||
                kernel.scheduler().find_thread(process.pid(), thread.tid) != &thread ||
                kernel.scheduler().current_thread() != &thread)
                return Status::denied("Linux UDP result lost its original captured frame");
            thread.syscall_pending = false;
            return set_linux_udp_result(process, thread.native_frame, value);
        };
        return linux_udp_operations.dispatch<lu::configured_native_profile()>(
            kernel, process, thread, access, begin, publish, runtime_failure);
    }
};

Status dispatch_linux_udp(Kernel &kernel, sched::ProcessControlBlock &process,
                          sched::ThreadControlBlock &thread)
{
    return LinuxUdpOperationRuntimeAuthority::dispatch(kernel, process, thread);
}
TcpImmediateRuntime tcp_immediate_runtime;

bool tcp_immediate_frame(Kernel &kernel, const sched::ProcessControlBlock &process, const sched::ThreadControlBlock &thread)
{
    return process.resources().personality == Personality::linux
        ? linux_tcp_direct_call<lu::configured_native_profile()>(thread.native_frame, kernel.scheduler(), process) != lu::TcpCall::none
        : native_tcp_call(hw::syscall(thread.native_frame), kernel.scheduler(), process) != lu::TcpCall::none;
}
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
namespace
{

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
        if (!kernel.scheduler().complete_wait(wait.ticket, sched::WaitCompletion::ready, udp_wake_source(ready)))
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
} // namespace
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

i64 native_udp_call(Kernel &kernel, sched::ProcessControlBlock *process, sched::ThreadControlBlock *caller,
                    hw::AddressSpace &space, syscall::Number number, const std::array<uptr, 6> &a,
                    bool &complete)
{
    using N = syscall::Number;
    // Native UAPI numbers and sockaddr layout are not Linux ABI values on
    // the ten supported targets. A Linux decoder must be added separately.
    if (process->resources().personality != Personality::native) return -38;
    static_assert(sizeof(ok_sockaddr_in) == 16);
    auto socket_description = [&](u64 descriptor) -> Result<u16> {
        if (descriptor > static_cast<u64>(__INT32_MAX__))
            return Status::bad_descriptor("native UDP descriptor is out of range");
        const auto *slot = process->fd_table().entry(static_cast<i32>(descriptor));
        const auto *open = slot && slot->used ? kernel.scheduler().open_description(slot->description) : nullptr;
        if (!open || !open->native_udp_handle)
            return Status::bad_descriptor("descriptor is not a native UDP socket");
        return slot->description;
    };
    auto address_from_user = [&](u64 pointer, u64 length) -> Result<ok_sockaddr_in> {
        if (length != sizeof(ok_sockaddr_in))
            return Status::invalid_argument("native UDP address must be sixteen bytes");
        ok_sockaddr_in address{};
        if (!hw::copy_from_user(space, &address, pointer, sizeof(address)))
            return Status::fault("native UDP address is unreadable");
        if (address.family != OK_AF_INET)
            return Status::invalid_argument("native UDP requires AF_INET");
        for (auto byte : address.reserved)
            if (byte) return Status::invalid_argument("native UDP address has nonzero reserved bytes");
        return address;
    };
    if (number == N::socket)
    {
        constexpr u64 allowed = OK_SOCK_DGRAM | OK_SOCK_NONBLOCK | OK_SOCK_CLOEXEC;
        if (a[0] != OK_AF_INET || (a[1] & ~allowed) ||
            (a[1] & OK_SOCK_DGRAM) != OK_SOCK_DGRAM ||
            (a[2] != 0 && a[2] != OK_IPPROTO_UDP)) return -22;
        if (auto status = kernel.native_udp_executor().status(); !status) return error(status);
        if (!kernel.native_udp().configured())
        {
            auto *extension = kernel.extension();
            if (!extension) return -38;
            auto link = extension->network_link(kernel);
            if (!link) return error(link.status());
            if (auto status = kernel.native_udp().configure(link.value()); !status)
                return error(status);
        }
        auto opened = kernel.native_udp().open();
        if (!opened) return error(opened.status());
        auto fd = kernel.posix().install_native_udp(opened.value(), (a[1] & OK_SOCK_CLOEXEC) != 0, (a[1] & OK_SOCK_NONBLOCK) != 0);
        if (!fd)
        {
            static_cast<void>(kernel.native_udp().close(opened.value()));
            return error(fd.status());
        }
        return fd.value();
    }
    const auto description_id = socket_description(a[0]);
    if (!description_id) return error(description_id.status());
    const Result<u32> handle = kernel.scheduler().open_description(description_id.value())->native_udp_handle;
    if (number == N::bind)
    {
        auto address = address_from_user(a[1], a[2]);
        if (!address) return error(address.status());
        const auto port = static_cast<u16>((u16{address.value().port_be[0]} << 8) |
                                           address.value().port_be[1]);
        const net::wire::Ipv4 local{address.value().address[0], address.value().address[1],
                                    address.value().address[2], address.value().address[3]};
        return error(kernel.native_udp().bind(handle.value(), local, port));
    }
    if (number == N::sendto)
    {
        auto *extension = kernel.extension();
        if (!extension) return -38;
        if (a[3]) return -22;
        if (auto status = kernel.native_udp_executor().status(); !status) return error(status);
        if (a[2] > net::NativeUdpService::datagram_capacity) return -90;
        auto address = address_from_user(a[4], a[5]);
        if (!address) return error(address.status());
        std::array<std::byte, net::NativeUdpService::datagram_capacity> payload{};
        if (!hw::copy_from_user(space, payload.data(), a[1], static_cast<usize>(a[2]))) return -14;
        std::array<std::byte, net::NativeUdpService::frame_capacity> frame{};
        const auto port = static_cast<u16>((u16{address.value().port_be[0]} << 8) |
                                           address.value().port_be[1]);
        const net::wire::Ipv4 destination{address.value().address[0], address.value().address[1],
                                           address.value().address[2], address.value().address[3]};
        auto length = kernel.native_udp().build_datagram(handle.value(), destination,
            port, {payload.data(), static_cast<usize>(a[2])}, frame);
        if (!length) return error(length.status());
        if (auto status = extension->network_transmit(kernel, {frame.data(), length.value()}); !status)
            return error(status);
        return static_cast<i64>(a[2]);
    }
    if (a[3]) return -22; // MSG_TRUNC and all other receive flags are unsupported.
    if (!!a[4] != !!a[5]) return -22;
    u32 address_capacity{};
    if (a[4])
    {
        if (!hw::copy_from_user(space, &address_capacity, a[5], sizeof(address_capacity))) return -14;
        if (address_capacity < sizeof(ok_sockaddr_in)) return -22;
        if (!hw::user_range(space, a[4], sizeof(ok_sockaddr_in), true) ||
            !hw::user_range(space, a[5], sizeof(address_capacity), true)) return -14;
    }
    const SyscallOrigins::NativeUdpReceiveRequest request{
        static_cast<i32>(a[0]), a[1], a[2], a[3], a[4], a[5], address_capacity};
    return begin_native_udp_receive(kernel, *process, *caller, description_id.value(), request, complete);
}
} // namespace runtime_detail
} // namespace ok::user::native
