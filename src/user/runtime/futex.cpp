// Futex wait/wake/requeue/wake-op, shared futex keys and robust-list retirement.
//
// Queues (spec 4.5): 64 hash buckets (futex_buckets.hpp), each a futex_bucket
// spin lock over the FIFO list of the waits queued on its keys. WAIT compares
// the user word and queues under the bucket, so a waker that changes the word
// before its WAKE either is seen by the compare or finds the wait queued. A
// WAKE unlinks each matching wait it visits and completes the ones still
// pending, counting only those; WAKE_OP and CMP_REQUEUE hold the caller's mm
// and both buckets. Wakers never write the owner's frame or free its record:
// the owner's resume (resume_futex) unlinks a wait a timeout or a cancel left
// queued, produces the result and retires the record.
#include "private.hpp"
#include "../futex_word.hpp"

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
bool same_futex_frame(const memory::PhysicalFrame &left, const memory::PhysicalFrame &right)
{
    return left.owner == right.owner && left.index == right.index &&
        left.generation == right.generation && left.address == right.address;
}
} // namespace
SyscallOrigins::FutexBinding futex_origin_binding(memory::ObjectHandle object, const FutexWait &wait)
{
    return {object, wait.ticket, wait.address,
        wait.domain == FutexKeyDomain::private_address_space ? 128U : 0U, wait.expected, wait.syscall,
        wait.timeout_address, wait.timeout.deadline(), wait.timeout.generation()};
}
constinit FutexObjects futex_objects{page_pool, sched::max_processes};
std::array<PollWaitIndex, memory::KernelEntryStackPool::capacity> futex_waits{};
constinit FutexBucketTable futex_buckets{};

// A digest of exactly the fields same_futex_key compares.
u32 futex_bucket_of(const FutexWait::Key &key)
{
    u32 digest = static_cast<u32>(key.domain) | (key.shared ? 0x100U : 0U);
    const auto mix = [&digest](u64 value) {
        for (unsigned half = 0; half < 2; ++half, value >>= 32)
            digest ^= static_cast<u32>(value) + 0x9e3779b9U + (digest << 6) + (digest >> 2);
    };
    if (key.shared)
    {
        mix(reinterpret_cast<uptr>(key.frame.owner));
        mix(static_cast<u64>(key.frame.index));
        mix(static_cast<u64>(key.frame.generation));
        mix(static_cast<u64>(key.word_offset));
    }
    else
    {
        mix(static_cast<u64>(key.pid));
        mix(static_cast<u64>(key.address));
    }
    return FutexBucketTable::index_of(digest);
}

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
Status stop_futex_runtime(Status status)
{
    runtime_failure = status;
    started = false;
    return status;
}
// The record's own unlink (spec 4.3 owner unlink): takes `wait` off the
// queue that lists it and runs `then(listed)` while that bucket is still
// held, or holding nothing when no queue lists it any more.
template <typename Then> auto owner_unlink(FutexWait &wait, Then &&then)
{
    const auto index = futex_buckets.lock_linked(wait);
    if (index == FutexBucketTable::none) return then(false);
    futex_buckets.unlink(index, wait);
    auto result = then(true);
    futex_buckets[index].lock.unlock();
    return result;
}
// WAKE's step for one listed wait, holding its bucket: the wait leaves the
// queue whatever happens next (the ticket is read first: after the unlink
// the record is its owner's alone), then completes if it is still pending.
// A wait that timed out, was interrupted or cancelled is not counted.
bool wake_listed(Kernel &kernel, u32 index, FutexWait &wait, u32 &kicks)
{
    const auto ticket = wait.ticket;
    futex_buckets.unlink(index, wait);
    return kernel.scheduler().complete_wait(ticket, sched::WaitCompletion::ready, sched::WakeSource::thread, kicks);
}
// Wakes the waits of `key` in queue order until `limit` of them woke.
usize wake_queued(Kernel &kernel, u32 index, const FutexWait::Key &key, usize limit, u32 &kicks)
{
    usize woken = 0;
    for (auto *wait = futex_buckets[index].head; wait != nullptr && woken < limit;)
    {
        auto *next = wait->bucket_next;
        if (same_futex_key(wait->key, key) && wake_listed(kernel, index, *wait, kicks)) ++woken;
        wait = next;
    }
    return woken;
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
    // Off its queue first: no waker reaches the record once it is unlisted.
    static_cast<void>(owner_unlink(*resolved.value(), [](bool listed) { return listed; }));
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
Status prepare_cancel_futex(sched::ProcessId pid)
{
    // cancel_futex stops the runtime on an object it cannot resolve; exec
    // finds that breach before it evicts sibling threads.
    for (const auto &entry : futex_waits)
        if (entry.pid == pid)
            if (auto resolved = futex_objects.resolve(entry.object); !resolved)
            {
                runtime_failure = resolved.status();
                started = false;
                return resolved.status();
            }
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
    if (!uaccess::user_range(process.resources().native_space, address, sizeof(u32),
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
                 !uaccess::user_range(process.resources().native_space, address, sizeof(u32), true))
            return Status::fault("ordinary private anonymous futex backing is read-only");
        return key;
    }
    return Status::fault("ordinary futex word has no native mapping record");
}

i64 wake_futex(Kernel &kernel, sched::ProcessId pid, uptr address, FutexKeyDomain domain, u32 limit);
// Resolves futex words of one address space to their backing frames.
struct FutexUserWords
{
    const hw::AddressSpace &space;
    Result<u32 *> operator()(uptr address, bool write) const { return uaccess::user_word(space, address, write); }
};
// A word already resolved: its pointer, valid while the caller holds the
// owning mm (or the kernel window with_user_word opened).
struct FutexHeldWord
{
    u32 *word;
    Result<u32 *> operator()(uptr, bool) const { return word; }
};
// WAKE_OP and REQUEUE hold the caller's mm around their resolutions and
// both buckets (spec 4.5): no translation writer changes the words then, so
// a failed resolution is a genuine fault and no retry is needed. Holding only
// the kernel lock, the caller first waits for its process's kernel sections,
// which walk the page records a resolution rewrites; with the kernel lock
// held no section of the process is admitted until it returns, so a
// resolution under mm finds none to wait for.
class FutexMmHold final
{
  public:
    explicit FutexMmHold(sched::ProcessControlBlock &process) : hold_(prepare(process)) {}

  private:
    static sched::AddressSpaceGate &prepare(sched::ProcessControlBlock &process)
    {
        if constexpr (smp_sections) drain_if_pinned(process.pid());
        return process.resources().mm;
    }
    sched::AddressSpaceGate::Hold hold_;
};
} // namespace
struct RobustUserAccess
{
    // load32 and compare_exchange32 are single atomic operations on the
    // backing frame.
    static constexpr bool indivisible_owner_update = true;
    Kernel &kernel;
    sched::ProcessControlBlock &process;
    static constexpr bool big_endian = lu::Codec<lu::configured_native_profile()>::profile.big_endian;

    Status read(u64 address, std::span<std::byte> bytes)
    {
        return uaccess::copy_from_user(process.resources().native_space, bytes.data(),
            static_cast<uptr>(address), bytes.size());
    }
    Result<u32> load32(u64 address)
    {
        const auto pointer = static_cast<uptr>(address);
        if (pointer % sizeof(u32)) return Status::fault("robust owner word is not aligned");
        return futex_word_load(FutexUserWords{process.resources().native_space}, pointer, big_endian);
    }
    Result<u32> compare_exchange32(u64 address, u32 expected, u32 desired)
    {
        const auto pointer = static_cast<uptr>(address);
        if (pointer % sizeof(u32)) return Status::fault("robust owner word is not writable and aligned");
        return futex_word_compare_exchange(FutexUserWords{process.resources().native_space}, pointer,
                                           expected, desired, big_endian);
    }
    void wake_one(u64 address)
    {
        const auto pointer = static_cast<uptr>(address);
        if (pointer % sizeof(u32) == 0 && uaccess::user_range(process.resources().native_space, pointer, sizeof(u32), true))
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
    const uptr address = tls.clear_tid_address;
    tls.clear_tid_address = 0;
    // Linux mm_release skips the store/wake for the last address-space user.
    // The store result is ignored. Shared anonymous key acquisition separately
    // requires a writable, aligned word, including when the store failed.
    if (peers && address)
    {
        auto &space = process.resources().native_space;
        // An aligned TID word is cleared by one atomic store, like a user store.
        if (address % sizeof(u32) == 0)
            static_cast<void>(futex_word_store(FutexUserWords{space}, address, 0, RobustUserAccess::big_endian));
        else
        {
            const u32 zero = 0;
            static_cast<void>(uaccess::copy_to_user(space, address, &zero, sizeof(zero)));
        }
        if (address % sizeof(u32) == 0 && uaccess::user_range(space, address, sizeof(u32), true))
            static_cast<void>(wake_futex(kernel, process.pid(), address, FutexKeyDomain::anonymous_shared_operation, 1));
    }
}
void clear_group_tids(Kernel &kernel, sched::ProcessControlBlock &process)
{
    usize live = 0;
    live += process.live_count();
    for (auto &thread : process.thread_records())
        if (thread.state != sched::ProcessState::exited)
        {
            static_cast<void>(retire_robust(kernel, process, thread.tls, thread.tid));
            clear_child_tid(kernel, process, thread.tls, live-- > 1);
        }
}
namespace
{
// The completed wait still names its captured syscall origin: the result
// goes into the frame that trapped.
Status validate_futex_origin(const sched::ProcessControlBlock &process, const sched::ThreadControlBlock &thread,
                             memory::ObjectHandle object, const FutexWait &wait)
{
    if (!wait.origin) return Status::success();
    const auto identity = syscall_origins.identity(process, thread);
    if (identity.capture != wait.origin ||
        !syscall_origins.validate_completed_futex(identity, thread.native_frame, futex_origin_binding(object, wait)))
        return Status::fault("futex wait lost its captured origin");
    return Status::success();
}
} // namespace
std::optional<i64> resume_futex(Kernel &kernel, sched::ProcessControlBlock &process,
                                sched::ThreadControlBlock &thread, memory::ObjectHandle object)
{
    if (!runtime_failure) return std::nullopt;
    auto resolved = futex_objects.resolve(object);
    if (!resolved)
    {
        stop_futex_runtime(resolved.status());
        return std::nullopt;
    }
    auto &wait = *resolved.value();
    if (wait.pid != process.pid() || wait.tid != thread.tid)
    {
        stop_futex_runtime(Status::fault("futex wait index lost its exact owner"));
        return std::nullopt;
    }
    // A timeout or a cancel completed the wait without unlinking it.
    static_cast<void>(owner_unlink(wait, [](bool listed) { return listed; }));
    // A record this continuation no longer owns is retired; nothing is
    // produced and the frame stays as it is. A replacement ticket belongs to
    // another continuation and is untouched.
    if (thread.wait_registration.active() || thread.wait_registration.ticket() != wait.ticket)
    {
        static_cast<void>(cancel_futex(kernel, object));
        return std::nullopt;
    }
    if (auto status = validate_futex_origin(process, thread, object, wait); !status)
    {
        stop_futex_runtime(status);
        return std::nullopt;
    }
    const auto cause = thread.wait_registration.completion();
    const i64 result = cause == sched::WaitCompletion::ready ? 0 : cause == sched::WaitCompletion::timeout ? -110 : -4;
    if (!cancel_futex(kernel, object, cause == sched::WaitCompletion::ready ? "ready" :
        cause == sched::WaitCompletion::timeout ? "timeout" : "cancelled", result)) return std::nullopt;
    return result;
}
bool wake_futex_object(Kernel &kernel, memory::ObjectHandle object)
{
    if (!runtime_failure) return false;
    auto resolved = futex_objects.resolve(object);
    if (!resolved) return false;
    u32 kicks = 0;
    const auto index = futex_buckets.lock_linked(*resolved.value());
    if (index == FutexBucketTable::none) return false;
    const bool woken = wake_listed(kernel, index, *resolved.value(), kicks);
    futex_buckets[index].lock.unlock();
    kernel.scheduler().notify_cpus(kicks);
    return woken;
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
    const auto index = futex_bucket_of(key.value());
    u32 kicks = 0;
    usize count = 0;
    {
        const FutexBucketTable::Guard guard{futex_buckets, index};
        count = wake_queued(kernel, index, key.value(), limit, kicks);
    }
    kernel.scheduler().notify_cpus(kicks);
    emit_shared_futex_wake(key.value(), count);
    return static_cast<i64>(count);
}
} // namespace
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
    const auto operation = lu::FutexWakeOperation::decode(request.encoded_operation);
    const FutexMmHold mm{*process};
    const auto first = resolve_futex_key(*process, request.first_address, request.domain);
    if (!first) return error(first.status());
    const auto second = resolve_futex_key(*process, request.second_address, request.domain);
    if (!second) return error(second.status());
    // Reject an undefined arithmetic operation without reading or modifying
    // user memory. Linux compares only after the modification has succeeded.
    if (operation.operation > 4) return -38;
    // Copy-on-write resolved under mm, so the frame stays the word's until
    // mm is released.
    const auto word = uaccess::user_word(process->resources().native_space, request.second_address, true);
    if (!word) return -14;
    const auto limit = [](u32 value) { return std::bit_cast<i32>(value) <= 0 ? 1U : value; };
    const auto first_index = futex_bucket_of(first.value()), second_index = futex_bucket_of(second.value());
    u32 kicks = 0;
    i64 result = 0;
    {
        const FutexBucketTable::PairGuard pair{futex_buckets, first_index, second_index};
        // One atomic read-modify-write of the backing frame: user atomics on
        // other CPUs are neither lost nor torn; the buckets order it against
        // every WAIT compare on either word.
        const auto previous = futex_word_wake_operation(FutexHeldWord{word.value()}, request.second_address,
                                                        operation, request.big_endian);
        if (!previous) return -14;
        const auto comparison = operation.compare(previous.value());
        if (!comparison) return error(comparison.status());
        result = static_cast<i64>(wake_queued(kernel, first_index, first.value(), limit(request.first_limit), kicks));
        if (comparison.value())
            result += static_cast<i64>(
                wake_queued(kernel, second_index, second.value(), limit(request.second_limit), kicks));
    }
    kernel.scheduler().notify_cpus(kicks);
    return result;
}

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
    const FutexMmHold mm{*process};
    const auto source = resolve_futex_key(*process, request.source_address, request.domain, FutexKeyAccess::read);
    if (!source) return error(source.status());
    const auto destination = resolve_futex_key(*process, request.destination_address, request.domain, FutexKeyAccess::read);
    if (!destination) return error(destination.status());
    u32 *compared = nullptr;
    if (request.compare)
    {
        auto word = uaccess::user_word(process->resources().native_space, request.source_address, false);
        if (!word) return -14;
        compared = word.value();
    }
    const auto from = futex_bucket_of(source.value()), to = futex_bucket_of(destination.value());
    const bool same_key = same_futex_key(source.value(), destination.value());
    // The pending waits chosen in queue order: the first `wakes` are woken,
    // the next `transfers` move to the destination.
    std::array<FutexWait *, futex_waits.size()> chosen{};
    usize wakes = 0, transfers = 0, woken = 0;
    u32 kicks = 0;
    {
        const FutexBucketTable::PairGuard pair{futex_buckets, from, to};
        if (compared)
        {
            // One atomic load with both buckets held: indivisible from the
            // requeue for every WAIT and WAKE on either word.
            const auto value = futex_word_load(FutexHeldWord{compared}, request.source_address, request.big_endian);
            if (!value) return -14;
            if (value.value() != request.expected) return -11;
        }
        for (auto *wait = futex_buckets[from].head; wait != nullptr;)
        {
            auto *next = wait->bucket_next;
            if ((wakes == request.wake_limit && transfers == request.requeue_limit) ||
                wakes + transfers == chosen.size())
                break;
            if (same_futex_key(wait->key, source.value()))
            {
                // A wait that timed out, was interrupted or cancelled leaves
                // the queue uncounted; its owner retires the record.
                if (!kernel.scheduler().wait_pending(wait->ticket)) futex_buckets.unlink(from, *wait);
                else if (wakes < request.wake_limit) chosen[wakes++] = wait;
                else chosen[wakes + transfers++] = wait;
            }
            wait = next;
        }
        if (!same_key && !futex_buckets.sequences_left(to, transfers))
            return error(Status::overflow("futex queue sequence exhausted"));
        // Every fallible new pin precedes the wakes and the moves; each one
        // has a discoverable waiter owner before the next fallible operation.
        for (usize index = wakes; index < wakes + transfers; ++index)
        {
            auto &wait = *chosen[index];
            if (!destination.value().shared || (wait.pin_owned && same_futex_frame(wait.key.frame, destination.value().frame)))
                continue;
            if (auto status = page_pool.pin(destination.value().frame); !status)
            {
                for (usize undo = wakes; undo < index; ++undo)
                {
                    auto &pinned = *chosen[undo];
                    if (!pinned.pending_transfer_pin_owned) continue;
                    if (auto failure = page_pool.unpin(pinned.pending_transfer_frame); !failure)
                    {
                        stop_futex_runtime(failure);
                        return -5;
                    }
                    pinned.pending_transfer_pin_owned = false;
                    pinned.pending_transfer_frame = {};
                }
                return error(status);
            }
            wait.pending_transfer_frame = destination.value().frame;
            wait.pending_transfer_pin_owned = true;
        }
        for (usize index = 0; index < wakes; ++index)
            if (wake_listed(kernel, from, *chosen[index], kicks)) ++woken;
        for (usize index = wakes; index < wakes + transfers; ++index)
        {
            auto &wait = *chosen[index];
            const bool reuse = wait.pin_owned && destination.value().shared &&
                same_futex_frame(wait.key.frame, destination.value().frame);
            if (wait.pin_owned && !reuse)
            {
                if (auto status = page_pool.unpin(wait.key.frame); !status)
                {
                    stop_futex_runtime(status);
                    transfers = 0;
                    break;
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
            // A requeue onto its own key keeps the wait's place and number.
            if (!same_key) futex_buckets.move(from, to, wait);
#if defined(OK_ENABLE_TEST_POINTS) && defined(OK_PRIVATE_SIGNAL_DELIVERY_FIXTURE)
            emit("OK_NATIVE_FUTEX_REQUEUE_TRANSFER pid="); emit_number(wait.pid); emit(" tid="); emit_number(wait.tid);
            emit(" wait="); emit_number(wait.ticket.generation); emit(" origin="); emit_number(wait.origin);
            emit(" address="); emit_number(wait.address); emit(" destination="); emit_number(request.destination_address);
            emit(" sequence="); emit_number(wait.queue_sequence); emit(" key_shared="); emit_number(wait.key.shared);
            emit("\n");
#endif
        }
    }
    kernel.scheduler().notify_cpus(kicks);
    if (!runtime_failure) return -5;
    return static_cast<i64>(woken + transfers);
}

namespace
{
// WAIT's compare-and-queue under the bucket (spec 4.5), one attempt.
enum class FutexQueueAttempt : u8
{
    queued,
    failed,        // `result` holds the syscall's error.
    needs_resolve, // The word is not accessible now: resolve and retry.
};
} // namespace

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
    // Everything fallible that needs no bucket comes first: the index slot,
    // the frame pin and the record.
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
#ifndef OK_KERNEL_FREESTANDING
    if (auto status = activate_pool(); !status)
        return error(status);
#endif
    const bool pinned = key.value().shared;
    if (pinned)
        if (auto status = page_pool.pin(key.value().frame); !status)
            return error(status);
    auto rollback_pin = [&](i64 result) -> i64
    {
        if (pinned)
            if (auto status = page_pool.unpin(key.value().frame); !status)
            {
                runtime_failure = status;
                started = false;
                return -5;
            }
        return result;
    };
    auto allocated = futex_objects.create();
    if (!allocated)
        return rollback_pin(error(allocated.status()));
    const auto object = allocated.value();
    // The record was never queued: it goes without an owner unlink.
    auto rollback = [&](i64 result) -> i64
    {
        if (auto status = futex_objects.destroy(object); !status)
        { runtime_failure = status; started = false; return -5; }
        if (auto status = futex_objects.trim_empty_pages(); !status)
        { runtime_failure = status; started = false; return -5; }
        return rollback_pin(result);
    };
    auto resolved = futex_objects.resolve(object);
    if (!resolved)
        return rollback(error(resolved.status()));
    auto &wait = *resolved.value();
    wait.pid = pid;
    wait.tid = current->tid;
    wait.address = request.address;
    wait.domain = request.domain;
    wait.key = key.value();
    wait.pin_owned = pinned;
    wait.timeout = deadline;
    wait.expected = request.value;
    const auto &space = process->resources().native_space;
    const auto index = futex_bucket_of(key.value());
    i64 result = 0;
    // Under the bucket: one atomic load of the word, the compare, the arm
    // (bucket -> task, then the deadline) and the queueing. A waker that
    // changes the word and then wakes either is seen here or finds the wait.
    const auto attempt = [&]() -> FutexQueueAttempt {
        const FutexBucketTable::Guard guard{futex_buckets, index};
        u32 value = 0;
        const auto access = uaccess::with_user_word(space, request.address, uaccess::WordAccess::read, [&](u32 &word) {
            value = futex_word_order(FutexWordAccess<futex_word_hardware_atomic>::load(&word), request.big_endian);
        });
        if (access == uaccess::WordResult::needs_resolve) return FutexQueueAttempt::needs_resolve;
        if (access == uaccess::WordResult::misaligned) { result = -22; return FutexQueueAttempt::failed; }
        if (value != request.value) { result = -11; return FutexQueueAttempt::failed; }
        if (request.timed)
        {
            auto now = arch::monotonic_nanoseconds();
            if (!now) { result = error(now.status()); return FutexQueueAttempt::failed; }
            if (now.value() >= deadline.deadline()) { result = -110; return FutexQueueAttempt::failed; }
        }
        if (!futex_buckets.sequences_left(index, 1))
        {
            result = error(Status::overflow("futex queue sequence exhausted"));
            return FutexQueueAttempt::failed;
        }
        auto registration = kernel.scheduler().register_current_wait(sched::ThreadWaitReason::futex, request.address);
        if (!registration) { result = error(registration.status()); return FutexQueueAttempt::failed; }
        wait.ticket = registration.value();
        if (request.timed)
            static_cast<void>(kernel.scheduler().arm_wait_deadline(wait.ticket, deadline.deadline()));
        futex_buckets.link_tail(index, wait);
        return FutexQueueAttempt::queued;
    };
    // A word that is not accessible now is resolved without the bucket and
    // tried again; the last attempt holds mm, under which no translation
    // changes, so it queues or meets a genuine fault.
    auto outcome = attempt();
    for (usize retry = 0; outcome == FutexQueueAttempt::needs_resolve && retry < uaccess::max_retries; ++retry)
    {
        if (!uaccess::user_range(space, request.address, sizeof(u32), false))
            return rollback(-14);
        uaccess::detail::count(&smp::EvidenceCpu::uaccess_retries);
        outcome = attempt();
    }
    if (outcome == FutexQueueAttempt::needs_resolve)
    {
        uaccess::detail::count(&smp::EvidenceCpu::uaccess_held);
        const FutexMmHold mm{*process};
        outcome = attempt();
        if (outcome == FutexQueueAttempt::needs_resolve) result = -14;
    }
    if (outcome != FutexQueueAttempt::queued)
        return rollback(result);
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
