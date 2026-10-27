// Futex wait/wake/requeue/wake-op, shared futex keys and robust-list retirement.
#include "private.hpp"

namespace ok::user::native
{
inline namespace runtime_detail
{
namespace
{
bool same_futex_key(const FutexWait::Key &left, const FutexWait::Key &right)
{
    if (left.domain != right.domain || left.shared != right.shared) return false;
    if (!left.shared) return left.pid == right.pid && left.address == right.address;
    return left.frame.owner == right.frame.owner && left.frame.index == right.frame.index &&
           left.frame.generation == right.frame.generation && left.word_offset == right.word_offset;
}
bool exact_futex_key(const FutexWait::Key &left, const FutexWait::Key &right)
{
    return left.domain == right.domain && left.pid == right.pid && left.address == right.address &&
           left.frame.owner == right.frame.owner && left.frame.index == right.frame.index &&
           left.frame.generation == right.frame.generation && left.frame.address == right.frame.address &&
           left.word_offset == right.word_offset && left.shared == right.shared;
}
} // namespace
SyscallOrigins::FutexBinding futex_origin_binding(memory::ObjectHandle object, const FutexWait &wait)
{
    return {object, wait.ticket, wait.address,
        wait.domain == FutexKeyDomain::private_address_space ? 128U : 0U, wait.expected, wait.syscall,
        wait.timeout_address, wait.timeout.deadline(), wait.timeout.generation()};
}
constinit FutexObjects futex_objects{page_pool, sched::max_processes};
std::array<PollWaitIndex, sched::max_processes> futex_waits{};
namespace
{
u64 futex_queue_sequence{};
Result<u64> reserve_futex_sequences(usize count)
{
    if (count > ~u64{0} - futex_queue_sequence)
        return Status::overflow("futex queue sequence exhausted");
    if (!count) return u64{0};
    const auto first = futex_queue_sequence + 1;
    futex_queue_sequence += count;
    return first;
}
} // namespace

[[maybe_unused]] memory::ObjectHandle futex_object(sched::ProcessId pid, sched::ThreadId tid)
{
    for (const auto &entry : futex_waits)
        if (entry.pid == pid && entry.tid == tid) return entry.object;
    return {};
}
namespace
{
void emit_futex_arm(const FutexWait &wait, uptr pc)
{
#if defined(OK_PRIVATE_SIGNAL_DELIVERY_FIXTURE) && defined(OK_ENABLE_TEST_POINTS)
    if (!wait.origin) return;
    using Codec = lu::Codec<lu::configured_native_profile()>;
    const bool time64 = Codec::profile.word_bytes == 8 ||
        Codec::syscall_number(lu::Operation::futex_time64) == wait.syscall;
    emit("OK_NATIVE_FUTEX_ARM pid="); emit_number(wait.pid); emit(" tid="); emit_number(wait.tid);
    emit(" wait="); emit_number(wait.ticket.generation); emit(" origin="); emit_number(wait.origin);
    emit(" pc="); emit_number(pc); emit(" syscall="); emit_number(wait.syscall);
    emit(" address="); emit_number(wait.address); emit(" operation=");
    emit_number(wait.domain == FutexKeyDomain::private_address_space ? 128 : 0);
    emit(" expected="); emit_number(wait.expected); emit(" timeout="); emit_number(wait.timeout_address);
    emit(" deadline="); emit_number(wait.timeout.deadline()); emit(time64 ? " format=time64" : " format=time32");
    emit(" live="); emit_number(futex_objects.accounting().live_objects); emit("\n");
#else
    static_cast<void>(wait); static_cast<void>(pc);
#endif
}
void emit_shared_futex_arm(const FutexWait &wait)
{
#ifdef OK_ENABLE_TEST_POINTS
    if (!wait.key.shared) return;
    emit("OK_SHARED_FUTEX_ARM pid="); emit_number(wait.pid);
    emit(" tid="); emit_number(wait.tid);
    emit(" owner="); emit_number(reinterpret_cast<uptr>(wait.key.frame.owner));
    emit(" index="); emit_number(wait.key.frame.index);
    emit(" generation="); emit_number(wait.key.frame.generation);
    emit(" offset="); emit_number(wait.key.word_offset);
    emit(" ticket="); emit_number(wait.ticket.generation);
    emit(" pinned="); emit_number(page_pool.pinned_count()); emit("\n");
#else
    static_cast<void>(wait);
#endif
}
void emit_shared_futex_wake(const FutexWait::Key &key, usize count)
{
#ifdef OK_ENABLE_TEST_POINTS
    if (!key.shared) return;
    emit("OK_SHARED_FUTEX_WAKE pid="); emit_number(key.pid);
    emit(" owner="); emit_number(reinterpret_cast<uptr>(key.frame.owner));
    emit(" index="); emit_number(key.frame.index);
    emit(" generation="); emit_number(key.frame.generation);
    emit(" offset="); emit_number(key.word_offset);
    emit(" count="); emit_number(count);
    emit(" pinned="); emit_number(page_pool.pinned_count()); emit("\n");
#else
    static_cast<void>(key); static_cast<void>(count);
#endif
}
} // namespace
Status cancel_futex(Kernel &kernel, memory::ObjectHandle object, std::string_view reason, i64 result)
{
    auto resolved = futex_objects.resolve(object);
    if (!resolved)
    {
        runtime_failure = resolved.status();
        started = false;
        return resolved.status();
    }
    const auto wait = *resolved.value();
    const auto ticket = wait.ticket;
    syscall_origins.retire_futex(wait.pid, wait.tid, futex_origin_binding(object, wait));
    if (ticket.generation)
        static_cast<void>(kernel.scheduler().complete_wait(ticket, sched::WaitCompletion::cancelled));
    if (wait.pin_owned)
    {
        // Keep the object and index discoverable if the allocator rejects the
        // retained generation. Native execution stops with that exact owner.
        if (auto status = page_pool.unpin(wait.key.frame); !status)
        {
            runtime_failure = status;
            started = false;
            return status;
        }
        resolved.value()->pin_owned = false;
    }
    if (resolved.value()->pending_transfer_pin_owned)
    {
        if (auto status = page_pool.unpin(resolved.value()->pending_transfer_frame); !status)
        {
            runtime_failure = status;
            started = false;
            return status;
        }
        resolved.value()->pending_transfer_pin_owned = false;
        resolved.value()->pending_transfer_frame = {};
    }
    for (auto &entry : futex_waits)
        if (entry.object == object)
            entry = {};
    if (auto status = futex_objects.destroy(object); !status)
    { runtime_failure = status; started = false; return status; }
    if (auto status = futex_objects.trim_empty_pages(); !status)
    { runtime_failure = status; started = false; return status; }
#if defined(OK_PRIVATE_SIGNAL_DELIVERY_FIXTURE) && defined(OK_ENABLE_TEST_POINTS)
    if (wait.origin)
    {
        emit("OK_NATIVE_FUTEX_RETIRE pid="); emit_number(wait.pid); emit(" tid="); emit_number(wait.tid);
        emit(" wait="); emit_number(ticket.generation); emit(" origin="); emit_number(wait.origin);
        emit(" reason="); emit(reason); emit(" result=");
        if (result < 0) { emit("-"); emit_number(static_cast<u64>(-(result + 1)) + 1); }
        else emit_number(static_cast<u64>(result));
        emit(" live="); emit_number(futex_objects.accounting().live_objects); emit("\n");
    }
#else
    static_cast<void>(reason); static_cast<void>(result);
#endif
    return Status::success();
}
Status cancel_futex(Kernel &kernel, sched::ProcessId pid)
{
    for (const auto &entry : futex_waits)
        if (entry.pid == pid)
            if (auto status = cancel_futex(kernel, entry.object); !status) return status;
    return Status::success();
}
enum class FutexKeyAccess : u8 { retained_write, read };
namespace
{
Result<FutexWait::Key> resolve_futex_key(const sched::ProcessControlBlock &process,
                                        uptr address, FutexKeyDomain domain,
                                        FutexKeyAccess access = FutexKeyAccess::retained_write)
{
    if (address % sizeof(u32)) return Status::invalid_argument("futex word is misaligned");
    if (address > hw::user_end - sizeof(u32)) return Status::fault("futex word exceeds user bounds");
    FutexWait::Key key{};
    key.domain = domain;
    key.pid = process.pid();
    key.address = address;
    if (domain == FutexKeyDomain::private_address_space)
        return key;
    if (!hw::user_range(process.resources().native_space, address, sizeof(u32),
                        access == FutexKeyAccess::retained_write))
        return Status::fault("ordinary futex word has invalid access");
    for (const auto &page : process.resources().native_pages)
    {
        if (address < page.virtual_address || address - page.virtual_address > hw::page_size - sizeof(u32))
            continue;
        const auto *frame = owned_frame(process.resources().native_pages, page);
        if (!frame || !frame->owner || !frame->generation || frame->address != page.physical_address)
            return Status::fault("ordinary futex backing lost canonical owner");
        if (shared_pool_page(process.resources().native_pages, page))
        {
            key.shared = true;
            key.frame = *frame;
            key.word_offset = address - page.virtual_address;
        }
        else if (access == FutexKeyAccess::read &&
                 !hw::user_range(process.resources().native_space, address, sizeof(u32), true))
            return Status::fault("ordinary private anonymous futex backing is read-only");
        return key;
    }
    return Status::fault("ordinary futex word has no native mapping record");
}

i64 wake_futex(Kernel &kernel, sched::ProcessId pid, uptr address, FutexKeyDomain domain, u32 limit);
} // namespace
struct RobustUserAccess
{
    static constexpr bool serialized_native_execution = true;
    Kernel &kernel;
    sched::ProcessControlBlock &process;
    static constexpr bool big_endian = lu::Codec<lu::configured_native_profile()>::profile.big_endian;

    Status read(u64 address, std::span<std::byte> bytes)
    {
        return hw::copy_from_user(process.resources().native_space, bytes.data(),
            static_cast<uptr>(address), bytes.size());
    }
    Result<u32> compare_exchange32(u64 address, u32 expected, u32 desired)
    {
        const auto pointer = static_cast<uptr>(address);
        auto &space = process.resources().native_space;
        if (pointer % sizeof(u32) || !hw::user_range(space, pointer, sizeof(u32), true))
            return Status::fault("robust owner word is not writable and aligned");
        std::array<std::byte, 4> bytes{};
        if (auto status = read(address, bytes); !status) return status;
        u32 actual{};
        for (usize i = 0; i < bytes.size(); ++i)
            actual |= static_cast<u32>(bytes[i]) << (8 * (big_endian ? 3 - i : i));
        if (actual != expected) return actual;
        for (usize i = 0; i < bytes.size(); ++i)
            bytes[i] = static_cast<std::byte>(desired >> (8 * (big_endian ? 3 - i : i)));
        // Every native user is stopped at this CPU0 service boundary. Checked
        // copy is indivisible relative to those users, not an SMP atomic CAS.
        if (auto status = hw::copy_to_user(space, pointer, bytes.data(), bytes.size()); !status) return status;
        return actual;
    }
    void wake_one(u64 address)
    {
        const auto pointer = static_cast<uptr>(address);
        if (pointer % sizeof(u32) == 0 && hw::user_range(process.resources().native_space, pointer, sizeof(u32), true))
            static_cast<void>(wake_futex(kernel, process.pid(), pointer, FutexKeyDomain::anonymous_shared_operation, 1));
    }
};
static_assert(sched::max_task_id <= robust_tid_mask);
RobustWalkReport retire_robust(Kernel &kernel, sched::ProcessControlBlock &process,
                              sched::ThreadLocalState &tls, sched::ThreadId owner)
{
    if (process.resources().personality != Personality::linux) return {};
    const uptr head = tls.robust_list;
    tls.robust_list = 0;
    tls.robust_list_length = 0;
    RobustUserAccess access{kernel, process};
    return retire_robust_list<lu::configured_native_profile()>(head, static_cast<u32>(owner), access);
}
void clear_child_tid(Kernel &kernel, sched::ProcessControlBlock &process, sched::ThreadLocalState &tls, bool peers)
{
    if (process.resources().personality != Personality::linux)
        return;
    const u32 zero = 0;
    const uptr address = tls.clear_tid_address;
    tls.clear_tid_address = 0;
    // Linux mm_release skips the store/wake for the last address-space user.
    // The store result is ignored. Shared anonymous key acquisition separately
    // requires a writable, aligned word, including when the store failed.
    if (peers && address)
    {
        static_cast<void>(hw::copy_to_user(process.resources().native_space, address, &zero, sizeof(zero)));
        if (address % sizeof(u32) == 0 && hw::user_range(process.resources().native_space, address, sizeof(u32), true))
            static_cast<void>(wake_futex(kernel, process.pid(), address, FutexKeyDomain::anonymous_shared_operation, 1));
    }
}
void clear_group_tids(Kernel &kernel, sched::ProcessControlBlock &process)
{
    usize live = 0;
    for (const auto &thread : process.threads()) live += thread.state != sched::ProcessState::exited;
    for (auto &thread : process.threads())
        if (thread.state != sched::ProcessState::exited)
        {
            static_cast<void>(retire_robust(kernel, process, thread.tls, thread.tid));
            clear_child_tid(kernel, process, thread.tls, live-- > 1);
        }
}
struct FutexQueueEntry
{
    memory::ObjectHandle object{};
    u64 sequence{};
    FutexWait::Key key{};
    sched::ProcessId pid{};
    sched::ThreadId tid{};
    SyscallOrigins::FutexBinding binding{};
    u64 origin{};
    SyscallOrigins::Identity origin_identity{};
};
struct FutexQueueSnapshot
{
    std::array<FutexQueueEntry, sched::max_processes> entries{};
    usize count{};
};
namespace
{
Status stop_futex_runtime(Status status)
{
    runtime_failure = status;
    started = false;
    return status;
}
bool pending_futex_wait(Kernel &kernel, const FutexWait &wait)
{
    const auto *process = kernel.scheduler().find(wait.pid);
    const auto *thread = kernel.scheduler().find_thread(wait.pid, wait.tid);
    return process && thread && thread->native_active && thread->syscall_pending &&
        process->state() != sched::ProcessState::zombie && process->state() != sched::ProcessState::exited &&
        thread->state == sched::ProcessState::blocked && thread->wait_reason == sched::ThreadWaitReason::futex &&
        thread->wait_key == wait.address && wait.ticket.owner == wait.pid && wait.ticket.thread == wait.tid &&
        thread->wait_registration.pending(wait.ticket);
}
Status validate_futex_origin(Kernel &kernel, memory::ObjectHandle object, const FutexWait &wait)
{
    if (!wait.origin) return Status::success();
    auto *process = kernel.scheduler().find(wait.pid);
    auto *thread = kernel.scheduler().find_thread(wait.pid, wait.tid);
    if (!process || !thread) return Status::fault("futex queue lost captured thread");
    const auto identity = syscall_origins.identity(*process, *thread);
    if (identity.capture != wait.origin ||
        !syscall_origins.validate_bound(identity, thread->native_frame, futex_origin_binding(object, wait)))
        return Status::fault("futex queue lost captured origin");
    return Status::success();
}
Result<FutexQueueSnapshot> snapshot_futex_queue(Kernel &kernel, const FutexWait::Key &key)
{
    if (!runtime_failure) return runtime_failure;
    FutexQueueSnapshot snapshot;
    for (const auto &entry : futex_waits)
    {
        if (!entry.pid) continue;
        auto resolved = futex_objects.resolve(entry.object);
        if (!resolved) return stop_futex_runtime(resolved.status());
        const auto &wait = *resolved.value();
        if (wait.pid != entry.pid || wait.tid != entry.tid)
            return stop_futex_runtime(Status::fault("futex queue index lost exact owner"));
        if (!same_futex_key(wait.key, key) || !pending_futex_wait(kernel, wait)) continue;
        if (!wait.queue_sequence || wait.pending_transfer_pin_owned || wait.pin_owned != wait.key.shared)
            return stop_futex_runtime(Status::fault("futex queue has invalid sequence or pending transfer"));
        if (auto status = validate_futex_origin(kernel, entry.object, wait); !status)
            return stop_futex_runtime(status);
        if (snapshot.count == snapshot.entries.size())
            return stop_futex_runtime(Status::overflow("futex queue snapshot exceeds capacity"));
        usize position = snapshot.count++;
        while (position && snapshot.entries[position - 1].sequence > wait.queue_sequence)
        {
            snapshot.entries[position] = snapshot.entries[position - 1];
            --position;
        }
        if ((position && snapshot.entries[position - 1].sequence == wait.queue_sequence) ||
            (position + 1 < snapshot.count && snapshot.entries[position + 1].sequence == wait.queue_sequence))
            return stop_futex_runtime(Status::fault("futex queue sequence was reused"));
        SyscallOrigins::Identity origin_identity{};
        if (wait.origin)
        {
            const auto *process = kernel.scheduler().find(wait.pid);
            const auto *thread = kernel.scheduler().find_thread(wait.pid, wait.tid);
            if (!process || !thread)
                return stop_futex_runtime(Status::fault("futex snapshot lost captured owner"));
            origin_identity = syscall_origins.identity(*process, *thread);
        }
        snapshot.entries[position] = {entry.object, wait.queue_sequence, wait.key, wait.pid, wait.tid,
            futex_origin_binding(entry.object, wait), wait.origin, origin_identity};
    }
    return snapshot;
}
Result<FutexWait *> selected_futex_wait(Kernel &kernel, const FutexQueueEntry &entry, const FutexWait::Key &key)
{
    auto resolved = futex_objects.resolve(entry.object);
    if (!resolved) return stop_futex_runtime(resolved.status());
    auto &wait = *resolved.value();
    if (wait.queue_sequence != entry.sequence || !same_futex_key(wait.key, key) ||
        !exact_futex_key(wait.key, entry.key) || wait.pid != entry.pid || wait.tid != entry.tid ||
        futex_origin_binding(entry.object, wait) != entry.binding || wait.origin != entry.origin ||
        !pending_futex_wait(kernel, wait))
        return stop_futex_runtime(Status::fault("selected futex lost exact pending continuation"));
    if (entry.origin)
    {
        const auto *process = kernel.scheduler().find(entry.pid);
        const auto *thread = kernel.scheduler().find_thread(entry.pid, entry.tid);
        if (!process || !thread || syscall_origins.identity(*process, *thread) != entry.origin_identity ||
            !syscall_origins.validate_bound(entry.origin_identity, thread->native_frame, entry.binding))
            return stop_futex_runtime(Status::fault("selected futex lost original captured origin"));
    }
    return &wait;
}
} // namespace
bool finish_futex(Kernel &kernel, memory::ObjectHandle object, sched::WaitCompletion cause, i64 result)
{
    if (!runtime_failure) return false;
    auto resolved = futex_objects.resolve(object);
    if (!resolved)
    {
        runtime_failure = resolved.status();
        return false;
    }
    const auto wait = *resolved.value();
    auto *process = kernel.scheduler().find(wait.pid);
    auto *thread = kernel.scheduler().find_thread(wait.pid, wait.tid);
    const bool completed = process && thread && thread->native_active &&
                           thread->wait_reason == sched::ThreadWaitReason::futex && thread->wait_key == wait.address &&
                           kernel.scheduler().complete_wait(wait.ticket, cause);
    if (completed)
    {
        thread->syscall_pending = false;
        if (auto status = set_result(*process, thread->native_frame, result); !status)
        {
            runtime_failure = status;
            finish(kernel, wait.pid, 127);
            return false;
        }
    }
    if (auto status = cancel_futex(kernel, object, cause == sched::WaitCompletion::ready ? "ready" :
        cause == sched::WaitCompletion::timeout ? "timeout" : "cancelled", result); !status) return false;
    return completed;
}
namespace
{
i64 wake_futex(Kernel &kernel, sched::ProcessId pid, uptr address, FutexKeyDomain domain, u32 limit)
{
    if (!runtime_failure) return -5;
    auto *caller = kernel.scheduler().find(pid);
    if (!caller) return -22;
    auto key = resolve_futex_key(*caller, address, domain);
    if (!key) return error(key.status());
    auto snapshot = snapshot_futex_queue(kernel, key.value());
    if (!snapshot) return -5;
    usize count = 0;
    for (usize index = 0; index < snapshot.value().count && count < limit; ++index)
    {
        const auto &entry = snapshot.value().entries[index];
        if (!selected_futex_wait(kernel, entry, key.value())) return -5;
        if (!finish_futex(kernel, entry.object, sched::WaitCompletion::ready, 0))
        {
            if (runtime_failure) stop_futex_runtime(Status::fault("selected futex wake failed"));
            return -5;
        }
        if (!runtime_failure) return -5;
        ++count;
    }
    emit_shared_futex_wake(key.value(), count);
    return static_cast<i64>(count);
}
} // namespace
bool expire_futex(Kernel &kernel, u64 now)
{
    if (!runtime_failure) return false;
    bool pending = false;
    for (const auto &entry : futex_waits)
    {
        if (!entry.pid)
            continue;
        const auto object = entry.object;
        auto resolved = futex_objects.resolve(object);
        if (!resolved)
        {
            runtime_failure = resolved.status();
            started = false;
            return false;
        }
        auto &wait = *resolved.value();
        auto *process = kernel.scheduler().find(wait.pid);
        auto *thread = kernel.scheduler().find_thread(wait.pid, wait.tid);
        // External scheduler cancellation owns the same completed ticket, but
        // has not encoded the saved syscall result. Never replay an old WAIT.
        // A replacement ticket belongs to another continuation and is untouched.
        if (process && thread && thread->native_active &&
            process->state() != sched::ProcessState::zombie && process->state() != sched::ProcessState::exited &&
            !thread->wait_registration.active() && thread->wait_registration.ticket() == wait.ticket &&
            (thread->state == sched::ProcessState::runnable || thread->state == sched::ProcessState::running))
        {
            const auto cause = thread->wait_registration.completion();
            const i64 result = cause == sched::WaitCompletion::ready ? 0 :
                               cause == sched::WaitCompletion::timeout ? -110 : -4;
            thread->syscall_pending = false;
            if (auto status = set_result(*process, thread->native_frame, result); !status)
            {
                runtime_failure = status;
                finish(kernel, wait.pid, 127);
            }
            else cancel_futex(kernel, object, cause == sched::WaitCompletion::ready ? "ready" :
                cause == sched::WaitCompletion::timeout ? "timeout" : "cancelled", result);
            if (!runtime_failure) return false;
            continue;
        }
        if (!process || !thread || !thread->native_active || process->state() == sched::ProcessState::zombie ||
            process->state() == sched::ProcessState::exited || !thread->wait_registration.pending(wait.ticket) ||
            thread->wait_reason != sched::ThreadWaitReason::futex || thread->wait_key != wait.address)
        {
            if (!cancel_futex(kernel, object)) return false;
            continue;
        }
        if (wait.timeout.complete(now, wait.timeout.generation()))
            static_cast<void>(finish_futex(kernel, object, sched::WaitCompletion::timeout, -110));
        else
            pending = true;
        if (!runtime_failure) return false;
    }
    return pending;
}
} // namespace runtime_detail

i64 futex_wake_operation(Kernel &kernel, sched::ProcessId pid, const FutexWakeOperationRequest &request)
{
    if (!runtime_failure) return -5;
    auto *process = kernel.scheduler().find(pid);
    auto *current = kernel.scheduler().current_thread();
    if (!process || !current || current->owner != pid || current->state != sched::ProcessState::running ||
        kernel.scheduler().current_pid() != pid)
        return -22;
    if (request.domain != FutexKeyDomain::private_address_space &&
        request.domain != FutexKeyDomain::anonymous_shared_operation)
        return -38;
    if (request.first_address % sizeof(u32) || request.second_address % sizeof(u32)) return -22;
    if (request.first_address > hw::user_end - sizeof(u32) ||
        request.second_address > hw::user_end - sizeof(u32)) return -14;
    const auto first = resolve_futex_key(*process, request.first_address, request.domain);
    if (!first) return error(first.status());
    const auto second = resolve_futex_key(*process, request.second_address, request.domain);
    if (!second) return error(second.status());
    const auto &space = process->resources().native_space;
    const auto operation = lu::FutexWakeOperation::decode(request.encoded_operation);
    // Reject an undefined arithmetic operation without reading or modifying
    // user memory. Linux compares only after the modification has succeeded.
    if (operation.operation > 4) return -38;
    if (!hw::user_range(space, request.second_address, sizeof(u32), true)) return -14;
    std::array<u8, sizeof(u32)> bytes{};
    if (!hw::copy_from_user(space, bytes.data(), request.second_address, bytes.size())) return -14;
    u32 previous{};
    for (usize index = 0; index < bytes.size(); ++index)
        previous |= static_cast<u32>(bytes[index]) << (8 * (request.big_endian ? 3 - index : index));
    const auto modified = operation.modify(previous);
    if (!modified) return error(modified.status());
    for (usize index = 0; index < bytes.size(); ++index)
        bytes[index] = static_cast<u8>(modified.value() >> (8 * (request.big_endian ? 3 - index : index)));
    // There is no user/IRQ queue producer in this serialized boot-CPU span.
    // SMP needs an atomic user-memory operation and futex queue locking.
    if (!hw::copy_to_user(space, request.second_address, bytes.data(), bytes.size())) return -14;
    const auto comparison = operation.compare(previous);
    if (!comparison) return error(comparison.status());
    const auto limit = [](u32 value) { return std::bit_cast<i32>(value) <= 0 ? 1U : value; };
    const auto woke_first = wake_futex(kernel, pid, request.first_address, request.domain, limit(request.first_limit));
    if (woke_first < 0 || !comparison.value()) return woke_first;
    const auto woke_second = wake_futex(kernel, pid, request.second_address, request.domain, limit(request.second_limit));
    return woke_second < 0 ? woke_second : woke_first + woke_second;
}
namespace
{

bool same_futex_frame(const memory::PhysicalFrame &left, const memory::PhysicalFrame &right)
{
    return left.owner == right.owner && left.index == right.index &&
        left.generation == right.generation && left.address == right.address;
}
Status rollback_futex_transfer_pins(const FutexQueueSnapshot &snapshot, usize begin, usize count)
{
    for (usize index = begin; index < begin + count; ++index)
    {
        auto resolved = futex_objects.resolve(snapshot.entries[index].object);
        if (!resolved) return stop_futex_runtime(resolved.status());
        auto &wait = *resolved.value();
        if (!wait.pending_transfer_pin_owned) continue;
        if (auto status = page_pool.unpin(wait.pending_transfer_frame); !status)
            return stop_futex_runtime(status);
        wait.pending_transfer_pin_owned = false;
        wait.pending_transfer_frame = {};
    }
    return Status::success();
}
} // namespace
i64 futex_requeue(Kernel &kernel, sched::ProcessId pid, const FutexRequeueRequest &request)
{
    if (!runtime_failure) return -5;
    auto *process = kernel.scheduler().find(pid);
    auto *current = kernel.scheduler().current_thread();
    if (!process || !current || current->owner != pid || current->state != sched::ProcessState::running ||
        kernel.scheduler().current_pid() != pid) return -22;
#if defined(OK_ENABLE_TEST_POINTS) && defined(OK_PRIVATE_SIGNAL_DELIVERY_FIXTURE)
    emit("OK_NATIVE_FUTEX_REQUEUE_CALL pid="); emit_number(pid); emit(" tid="); emit_number(current->tid);
    emit(" source="); emit_number(request.source_address); emit(" destination="); emit_number(request.destination_address);
    emit(" wake_bits="); emit_number(request.wake_limit); emit(" requeue_bits="); emit_number(request.requeue_limit);
    emit(" compare="); emit_number(request.compare); emit(" expected="); emit_number(request.expected);
    emit(" private="); emit_number(request.domain == FutexKeyDomain::private_address_space); emit("\n");
#endif
    if (request.domain != FutexKeyDomain::private_address_space &&
        request.domain != FutexKeyDomain::anonymous_shared_operation) return -38;
    if (std::bit_cast<i32>(request.wake_limit) < 0 || std::bit_cast<i32>(request.requeue_limit) < 0) return -22;
    const auto source = resolve_futex_key(*process, request.source_address, request.domain, FutexKeyAccess::read);
    if (!source) return error(source.status());
    const auto destination = resolve_futex_key(*process, request.destination_address, request.domain, FutexKeyAccess::read);
    if (!destination) return error(destination.status());
    if (request.compare)
    {
        std::array<u8, sizeof(u32)> bytes{};
        if (!hw::copy_from_user(process->resources().native_space, bytes.data(), request.source_address, bytes.size()))
            return -14;
        u32 value{};
        for (usize index = 0; index < bytes.size(); ++index)
            value |= static_cast<u32>(bytes[index]) << (8 * (request.big_endian ? 3 - index : index));
        if (value != request.expected) return -11;
    }
    auto selected = snapshot_futex_queue(kernel, source.value());
    if (!selected) return -5;
    const auto &snapshot = selected.value();
    const usize wakes = request.wake_limit < snapshot.count ? request.wake_limit : snapshot.count;
    const usize remaining = snapshot.count - wakes;
    const usize transfers = request.requeue_limit < remaining ? request.requeue_limit : remaining;
    const bool same_key = same_futex_key(source.value(), destination.value());
    auto sequences = reserve_futex_sequences(same_key ? 0 : transfers);
    if (!sequences) return error(sequences.status());
    // All fallible new pins precede wakes/key publication. Each successful pin
    // has a discoverable waiter owner before the next fallible operation.
    for (usize index = wakes; index < wakes + transfers; ++index)
    {
        auto resolved = selected_futex_wait(kernel, snapshot.entries[index], source.value());
        if (!resolved) return -5;
        auto &wait = *resolved.value();
        if (destination.value().shared &&
            (!wait.pin_owned || !same_futex_frame(wait.key.frame, destination.value().frame)))
        {
            if (auto status = page_pool.pin(destination.value().frame); !status)
            {
                if (!rollback_futex_transfer_pins(snapshot, wakes, transfers)) return -5;
                return error(status);
            }
            wait.pending_transfer_frame = destination.value().frame;
            wait.pending_transfer_pin_owned = true;
        }
    }
    for (usize index = 0; index < wakes; ++index)
    {
        if (!selected_futex_wait(kernel, snapshot.entries[index], source.value()) ||
            !finish_futex(kernel, snapshot.entries[index].object, sched::WaitCompletion::ready, 0))
        {
            if (runtime_failure) stop_futex_runtime(Status::fault("requeue wake lost exact continuation"));
            return -5;
        }
        if (!runtime_failure) return -5;
    }
    for (usize index = wakes; index < wakes + transfers; ++index)
    {
        auto resolved = selected_futex_wait(kernel, snapshot.entries[index], source.value());
        if (!resolved) return -5;
        auto &wait = *resolved.value();
        const bool reuse = wait.pin_owned && destination.value().shared &&
            same_futex_frame(wait.key.frame, destination.value().frame);
        if (wait.pin_owned && !reuse)
        {
            if (auto status = page_pool.unpin(wait.key.frame); !status)
            {
                stop_futex_runtime(status);
                return -5;
            }
            wait.pin_owned = false;
        }
        wait.key = destination.value();
        if (!reuse)
        {
            wait.pin_owned = wait.pending_transfer_pin_owned;
            wait.pending_transfer_pin_owned = false;
            wait.pending_transfer_frame = {};
        }
        if (!same_key) wait.queue_sequence = sequences.value() + (index - wakes);
#if defined(OK_ENABLE_TEST_POINTS) && defined(OK_PRIVATE_SIGNAL_DELIVERY_FIXTURE)
        emit("OK_NATIVE_FUTEX_REQUEUE_TRANSFER pid="); emit_number(wait.pid); emit(" tid="); emit_number(wait.tid);
        emit(" wait="); emit_number(wait.ticket.generation); emit(" origin="); emit_number(wait.origin);
        emit(" address="); emit_number(wait.address); emit(" destination="); emit_number(request.destination_address);
        emit(" sequence="); emit_number(wait.queue_sequence); emit(" key_shared="); emit_number(wait.key.shared);
        emit("\n");
#endif
    }
    return static_cast<i64>(wakes + transfers);
}

i64 futex(Kernel &kernel, sched::ProcessId pid, const FutexRequest &request, bool &complete)
{
    if (!runtime_failure) return -5;
    auto *process = kernel.scheduler().find(pid);
    auto *current = kernel.scheduler().current_thread();
    if (!process || !current || current->owner != pid || current->state != sched::ProcessState::running ||
        kernel.scheduler().current_pid() != pid)
        return -22;
    if (request.operation != FutexOperation::wait && request.operation != FutexOperation::wake)
        return -38;
    if (request.domain != FutexKeyDomain::private_address_space &&
        request.domain != FutexKeyDomain::anonymous_shared_operation)
        return -38;
    if (request.operation == FutexOperation::wait && request.timed &&
        (request.timeout.seconds < 0 || request.timeout.nanoseconds < 0 || request.timeout.nanoseconds >= 1000000000))
        return -22;
    sched::DeadlineWait deadline;
    if (request.operation == FutexOperation::wait && request.timed)
    {
        auto now = arch::monotonic_nanoseconds();
        if (!now)
            return error(now.status());
        const auto end =
            lu::linux_sleep_deadline(now.value(), {request.timeout.seconds, request.timeout.nanoseconds}, false);
        auto armed =
            deadline.arm(now.value(), static_cast<i64>(end / 1000000000), static_cast<i64>(end % 1000000000), true);
        if (!armed)
            return error(armed.status());
    }
    if (request.address % sizeof(u32))
        return -22;
    if (request.address > hw::user_end - sizeof(u32))
        return -14;
    auto key = resolve_futex_key(*process, request.address, request.domain);
    if (!key) return error(key.status());
    if (request.operation == FutexOperation::wake)
    {
        // The Linux legacy WAKE loop increments before comparing a signed
        // count. Zero and negative counts therefore wake one matching waiter.
        const u32 limit = static_cast<i32>(request.value) <= 0 ? 1 : request.value;
        return wake_futex(kernel, pid, request.address, request.domain, limit);
    }
    // No user execution or IRQ queue producer can run in this serialized span.
    // Do not generalize it to SMP without a lock and explicit memory ordering.
    std::array<u8, 4> bytes{};
    if (!hw::copy_from_user(process->resources().native_space, bytes.data(), request.address, bytes.size()))
        return -14;
    u32 value = 0;
    for (usize i = 0; i < bytes.size(); ++i)
        value |= static_cast<u32>(bytes[i]) << (8 * (request.big_endian ? 3 - i : i));
    if (value != request.value)
        return -11;
    if (request.timed)
    {
        auto now = arch::monotonic_nanoseconds();
        if (!now) return error(now.status());
        if (deadline.complete(now.value(), deadline.generation())) return -110;
    }
    PollWaitIndex *slot = nullptr;
    for (auto &entry : futex_waits)
    {
        if (entry.pid == pid && entry.tid == current->tid)
            return -22;
        if (!entry.pid && !slot)
            slot = &entry;
    }
    if (!slot)
        return -12;
    auto sequence = reserve_futex_sequences(1);
    if (!sequence) return error(sequence.status());
#ifndef OK_KERNEL_FREESTANDING
    if (auto status = activate_pool(); !status)
        return error(status);
#endif
    const bool pinned = key.value().shared;
    if (pinned)
        if (auto status = page_pool.pin(key.value().frame); !status)
            return error(status);
    auto rollback_pin = [&](Status failure) -> i64
    {
        if (pinned)
            if (auto status = page_pool.unpin(key.value().frame); !status)
            {
                runtime_failure = status;
                started = false;
                return -5;
            }
        return error(failure);
    };
    auto allocated = futex_objects.create();
    if (!allocated)
        return rollback_pin(allocated.status());
    const auto object = allocated.value();
    auto resolved = futex_objects.resolve(object);
    if (!resolved)
    {
        const auto failure = resolved.status();
        if (auto status = futex_objects.destroy(object); !status)
        { runtime_failure = status; started = false; return -5; }
        if (auto status = futex_objects.trim_empty_pages(); !status)
        { runtime_failure = status; started = false; return -5; }
        return rollback_pin(failure);
    }
    auto &wait = *resolved.value();
    wait.pid = pid;
    wait.tid = current->tid;
    wait.address = request.address;
    wait.domain = request.domain;
    wait.key = key.value();
    wait.pin_owned = pinned;
    wait.queue_sequence = sequence.value();
    wait.timeout = deadline;
    wait.expected = request.value;
    auto registration = kernel.scheduler().register_current_wait(sched::ThreadWaitReason::futex, request.address);
    if (!registration)
    {
        cancel_futex(kernel, object);
        return error(registration.status());
    }
    wait.ticket = registration.value();
    *slot = {pid, current->tid, object};
    const auto origin = syscall_origins.identity(*process, *current);
    if (syscall_origins.live(origin))
    {
        // Only the first four WAIT inputs exist on every profile. In particular,
        // o32 arguments five/six must not be fetched from an unused user stack.
        const auto call = SyscallOrigins::Adapter::Bridge::decode_call(current->native_frame, 4);
        if (call && (call.value().arguments[3] != 0) == request.timed)
        {
            wait.syscall = call.value().number;
            wait.timeout_address = call.value().arguments[3];
            if (syscall_origins.bind(origin, current->native_frame, futex_origin_binding(object, wait)))
                wait.origin = origin.capture;
        }
        if (!wait.origin)
        {
            syscall_origins.revoke(*current);
            if (pinned)
            {
                cancel_futex(kernel, object);
                return -5;
            }
        }
    }
    emit_futex_arm(wait, current->native_frame.pc);
    emit_shared_futex_arm(wait);
    complete = false;
    return 0;
}
} // namespace ok::user::native
