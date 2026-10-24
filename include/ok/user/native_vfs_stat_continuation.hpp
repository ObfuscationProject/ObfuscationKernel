#pragma once

#include "ok/abi/linux_native.hpp"
#include "ok/user/native_backend_capture.hpp"
#include "ok/abi/linux_stat_call.hpp"
#include "ok/memory/kernel_entry_stack_pool.hpp"
#include "ok/sched/scheduler.hpp"

namespace ok::user::native
{

// Private serialized service-loop prototype. This owns copied statx requests;
// it neither parks a kernel stack nor publishes a signal instruction restart.
template <abi::linux_user::Profile Selected>
    requires abi::linux_user::SupportedProfile<Selected>
class VfsStatContinuation final : private NonCopyable
{
  public:
    using Frame = arch::execution::UserFrame;
    using Space = arch::execution::AddressSpace;
    using Pool = memory::KernelEntryStackPool;
    using Lease = memory::KernelEntryStackHandle;
    using Bridge = abi::linux_user::NativeBridge<Selected>;
    using Stat = abi::linux_user::StatCallAdapter<Selected>;
    using Codec = abi::linux_user::Codec<Selected>;
    static constexpr usize capacity = Pool::capacity;
    struct Handle
    {
        const VfsStatContinuation *authority{};
        u32 slot{};
        u64 request{};
        [[nodiscard]] constexpr bool operator==(const Handle &) const = default;
    };

    explicit constexpr VfsStatContinuation(Pool &pool) : pool_(&pool), legacy_capture_(pool) {}

    Status prepare(const sched::ThreadControlBlock &thread)
    {
        if (auto status = pool_->validate_inactive(thread.entry_stack, {thread.owner, thread.tid}); !status)
            return status;
        auto &record = records_[thread.entry_stack.slot];
        if (record.pending) return Status::busy("VFS continuation must retire before image preparation");
        if (images_ == ~u64{0}) return Status::overflow("VFS continuation image sequence exhausted");
        record = {};
        record.lease = thread.entry_stack;
        record.pid = thread.owner;
        record.tid = thread.tid;
        record.image = ++images_;
        return {};
    }

    class CaptureStage final
    {
      public:
        CaptureStage(const CaptureStage &) = delete;
        CaptureStage &operator=(const CaptureStage &) = delete;
        CaptureStage &operator=(CaptureStage &&) = delete;
        CaptureStage(CaptureStage &&other) noexcept
            : owner_(std::exchange(other.owner_, nullptr)), lease_(other.lease_), image_(std::exchange(other.image_, 0)), epoch_(other.epoch_) {}
      private:
        friend class VfsStatContinuation;
        CaptureStage(VfsStatContinuation &owner, Lease lease, u64 image, u64 epoch)
            : owner_(&owner), lease_(lease), image_(image), epoch_(epoch) {}
        VfsStatContinuation *owner_{};
        Lease lease_{};
        u64 image_{}, epoch_{};
    };
    Result<CaptureStage> stage_capture(sched::ProcessControlBlock &process, sched::ThreadControlBlock &thread,
                                      const arch::execution::KernelEntryStackView &view, sched::SchedulerState &scheduler)
    {
        auto *record = owned(thread);
        if (!record || !record->image || record->pending || thread.syscall_pending || !thread.native_active ||
            thread.state != sched::ProcessState::running || scheduler.find(process.pid()) != &process ||
            scheduler.find_thread(thread.owner, thread.tid) != &thread || process.pid() != thread.owner)
            return Status::denied("VFS entry has no canonical prepared native owner");
        record->captured = false;
        const auto epoch = pool_->active_epoch(thread.entry_stack, {thread.owner, thread.tid}, view);
        if (!epoch) return epoch.status();
        return CaptureStage(*this, thread.entry_stack, record->image, epoch.value());
    }
    Status observe(CaptureStage &stage, const BackendCapture::Observation &observation,
                   sched::ProcessControlBlock &process, sched::ThreadControlBlock &thread,
                   const arch::execution::KernelEntryStackView &view, sched::SchedulerState &scheduler)
    {
        auto *record = owned(thread);
        if (stage.owner_ != this || !stage.image_ || !record || record->image != stage.image_ ||
            stage.lease_ != thread.entry_stack || stage.lease_ != observation.lease() || stage.epoch_ != observation.epoch() ||
            !BackendCapture::claim(observation, BackendCapture::Observer::vfs_continuation, scheduler, process, thread, view))
            return Status::denied("VFS continuation has no shared owned capture stage");
        stage.owner_ = nullptr; stage.image_ = 0;
        if (observation.trap().kind != arch::execution::TrapKind::syscall || process.resources().personality != sched::UserPersonality::linux)
            return {};
        if (captures_ == ~u64{0}) return Status::overflow("VFS continuation capture sequence exhausted");
        record->capture = ++captures_;
        record->epoch = observation.epoch();
        record->space = observation.space();
        record->frame = observation.frame();
        record->captured = true;
        return {};
    }
    struct Ownership { usize prepared{}, pending{}, captured{}; };
    [[nodiscard]] Ownership ownership() const
    {
        Ownership result{};
        for (const auto &record : records_)
        {
            result.prepared += record.lease.pool != nullptr;
            result.pending += record.pending.has_value();
            result.captured += record.captured;
        }
        return result;
    }
    Status retire(const sched::ThreadControlBlock &thread)
    {
        if (auto *record = owned(thread))
        {
            if (record->pending) return Status::busy("VFS continuation retirement is still pending");
            *record = {};
        }
        return {};
    }
    void revoke(const sched::ThreadControlBlock &thread)
    { if (auto *record = owned(thread)) record->captured = false; }
    // Compatibility path for callers owning only the ordinary VFS observer.
    Result<arch::execution::Trap> enter(sched::SchedulerState &scheduler,
                                       sched::ProcessControlBlock &process,
                                       sched::ThreadControlBlock &thread,
                                       const arch::execution::KernelEntryStackView &view)
    {
        auto stage = stage_capture(process, thread, view, scheduler);
        if (!stage) return stage.status();
        auto observed = legacy_capture_.enter(scheduler, process, thread, view);
        if (!observed) return observed.status();
        if (auto status = observe(stage.value(), observed.value(), process, thread, view, scheduler); !status) return status;
        return observed.value().trap();
    }

    // Stack arguments and pathname are copied exactly once before admission.
    // This bounded executor accepts only statx with an absolute nonempty path.
    template <typename ReadPath, typename CopyStack, typename Admit, typename Copy>
    i64 begin(sched::SchedulerState &scheduler, fs::VirtualFileSystem &vfs,
              sched::ProcessId pid, sched::ThreadId tid, ReadPath read_path, CopyStack copy_stack,
              Admit admit, Copy copy, bool &complete)
    {
        auto *thread = scheduler.find_thread(pid, tid);
        auto *record = thread ? owned(*thread) : nullptr;
        if (!record || !live(scheduler, *record) || !current_running(scheduler, *record) || record->pending)
            return -125;
        const auto raw = Bridge::decode_call_copying_stack(record->frame, 5, copy_stack);
        const auto number = Codec::syscall_number(abi::linux_user::Operation::statx);
        if (!raw || !number || raw.value().number != *number)
            return terminal(*record, raw ? -95 : error(raw.status()));
        const auto decoded = Stat::decode(abi::linux_user::StatCall::statx,
                                          {raw.value().arguments.data(), 5});
        if (!decoded) return terminal(*record, error(decoded.status()));
        Pending request;
        request.operation = decoded.value();
        // No retained dirfd/cwd context is implied by this bounded prototype.
        if (request.operation.direct_fd || !request.operation.path || (request.operation.flags & posix::at_EMPTY_PATH))
            return terminal(*record, -95);
        if (auto status = read_path(request.operation.path, request.path); !status)
            return terminal(*record, error(status));
        if (request.path.empty() || request.path.view().front() != '/') return terminal(*record, -95);
        const auto *process = scheduler.find(pid);
        request.credentials = {process->credentials().euid, process->credentials().egid};
        if (requests_ == ~u64{0}) return terminal(*record, -75);
        request.sequence = ++requests_;
        request.vfs = &vfs;
        record->pending.emplace(std::move(request));
        return attempt(scheduler, *record, admit, copy, complete);
    }

    // There is deliberately no pathname/stack input callback on resume.
    template <typename Admit, typename Copy>
    i64 resume(sched::SchedulerState &scheduler, Handle handle, Admit admit, Copy copy, bool &complete)
    {
        auto *record = resolve(handle);
        if (!record) return -125;
        auto *thread = scheduler.find_thread(record->pid, record->tid);
        if (!live(scheduler, *record) || !current_running(scheduler, *record) || !thread ||
            thread->wait_registration.ticket() != record->pending->ticket || thread->wait_registration.active() ||
            thread->wait_registration.completion() != sched::WaitCompletion::ready ||
            thread->wait_reason != sched::ThreadWaitReason::none || thread->wait_key != 0)
        { static_cast<void>(cancel(scheduler, handle)); return -125; }
        return attempt(scheduler, *record, admit, copy, complete);
    }

    [[nodiscard]] Handle pending(sched::ProcessId pid, sched::ThreadId tid) const
    {
        for (usize slot = 0; slot < capacity; ++slot)
            if (const auto &r = records_[slot]; r.pid == pid && r.tid == tid && r.pending)
                return {this, static_cast<u32>(slot), r.pending->sequence};
        return {};
    }
    [[nodiscard]] sched::WaitTicket ticket(Handle handle) const
    {
        const auto *record = resolve(handle);
        return record ? record->pending->ticket : sched::WaitTicket{};
    }
    [[nodiscard]] usize pending_count(sched::ProcessId pid) const
    {
        usize count{};
        for (const auto &record : records_) if (record.pid == pid && record.pending) ++count;
        return count;
    }

    Status cancel(sched::SchedulerState &scheduler, Handle handle)
    {
        auto *record = resolve(handle);
        if (!record) return Status::not_found("VFS continuation cancellation handle is stale");
        const auto wait = record->pending->ticket;
        auto cleanup = record->pending->vfs->cancel_certified_operation_wait(wait, this);
        if (cleanup.code() == StatusCode::not_found) cleanup = {};
        if (!cleanup) return cleanup;
        // Invalidation of an old capture may retire its own wait, but cannot
        // cancel a replacement reason/key/ticket or another queue consumer.
        if (owns_active_wait(scheduler, *record) &&
            !scheduler.complete_wait(wait, sched::WaitCompletion::cancelled))
            return Status::fault("VFS cancellation lost its exact native wait");
        record->pending.reset();
        record->captured = false;
        return cleanup;
    }

    Status cancel_process(sched::SchedulerState &scheduler, sched::ProcessId pid)
    {
        Status result;
        for (usize slot = 0; slot < capacity; ++slot)
            if (auto &r = records_[slot]; r.pid == pid && r.pending)
            {
                auto status = cancel(scheduler, {this, static_cast<u32>(slot), r.pending->sequence});
                if (!status && result) result = status;
            }
        return result;
    }

    // Runtime must call this before thread selection, with the scheduler still
    // serialized. Readiness grants no VFS operation; resume reacquires it.
    Status drain(sched::SchedulerState &scheduler, fs::VirtualFileSystem &vfs)
    {
        for (;;)
        {
            auto ready = vfs.take_ready_certified_operation_wait(this);
            if (!ready) return ready.status().code() == StatusCode::not_found ? Status{} : ready.status();
            for (usize slot = 0; slot < capacity; ++slot)
            {
                auto &record = records_[slot];
                if (!record.pending || record.pending->vfs != &vfs || record.pending->ticket != ready.value()) continue;
                if (!live(scheduler, record) || !owns_active_wait(scheduler, record))
                {
                    if (auto status = cancel(scheduler, {this, static_cast<u32>(slot), record.pending->sequence}); !status)
                        return status;
                }
                else if (!scheduler.complete_wait(ready.value(), sched::WaitCompletion::ready))
                    return Status::fault("VFS drain lost its exact native wait");
                break;
            }
        }
    }

    Status prune(sched::SchedulerState &scheduler)
    {
        for (usize slot = 0; slot < capacity; ++slot)
        {
            auto &record = records_[slot];
            if (!record.pending) continue;
            const auto *thread = scheduler.find_thread(record.pid, record.tid);
            const bool wait_owned = thread && thread->wait_registration.ticket() == record.pending->ticket &&
                (owns_active_wait(scheduler, record) || (!thread->wait_registration.active() &&
                 thread->wait_registration.completion() == sched::WaitCompletion::ready &&
                 thread->wait_reason == sched::ThreadWaitReason::none && thread->wait_key == 0 &&
                 (thread->state == sched::ProcessState::runnable || thread->state == sched::ProcessState::running)));
            if (!live(scheduler, record) || !wait_owned)
                if (auto status = cancel(scheduler, {this, static_cast<u32>(slot), record.pending->sequence}); !status)
                    return status;
        }
        return {};
    }

  private:
#ifdef OK_NATIVE_VFS_CONTINUATION_MODEL
    friend struct VfsStatContinuationTestAccess;
#endif
    struct Pending
    {
        abi::linux_user::StatRequest operation{};
        FixedString<256> path{};
        fs::Credentials credentials{};
        fs::VirtualFileSystem *vfs{};
        sched::WaitTicket ticket{};
        u64 sequence{};
    };
    struct Record
    {
        Lease lease{};
        u64 pid{}, tid{}, image{}, capture{}, epoch{};
        Space space{};
        Frame frame{};
        std::optional<Pending> pending{};
        bool captured{};
    };
    [[nodiscard]] static bool same_space(Space a, Space b)
    { return a.root == b.root && a.slot == b.slot && a.generation == b.generation; }
    [[nodiscard]] static bool same_frame(const Frame &a, const Frame &b)
    {
        return a.registers == b.registers && a.pc == b.pc && a.sp == b.sp && a.flags == b.flags &&
            a.fs_base == b.fs_base && a.gs_base == b.gs_base && a.fp_state == b.fp_state;
    }
    [[nodiscard]] Record *owned(const sched::ThreadControlBlock &thread)
    {
        if (thread.entry_stack.pool != pool_ || thread.entry_stack.slot >= capacity) return nullptr;
        auto &r = records_[thread.entry_stack.slot];
        return r.lease == thread.entry_stack && r.pid == thread.owner && r.tid == thread.tid ? &r : nullptr;
    }
    [[nodiscard]] const Record *resolve(Handle handle) const
    {
        if (handle.authority != this || handle.slot >= capacity || !handle.request) return nullptr;
        const auto &r = records_[handle.slot];
        return r.pending && r.pending->sequence == handle.request ? &r : nullptr;
    }
    [[nodiscard]] Record *resolve(Handle handle)
    { return const_cast<Record *>(static_cast<const VfsStatContinuation *>(this)->resolve(handle)); }
    [[nodiscard]] bool live(sched::SchedulerState &scheduler, const Record &record)
    {
        const auto *process = scheduler.find(record.pid);
        const auto *thread = scheduler.find_thread(record.pid, record.tid);
        const auto epoch = pool_->inactive_epoch(record.lease, {record.pid, record.tid});
        return record.captured && record.image && record.capture && record.epoch && epoch &&
            epoch.value() == record.epoch && process && thread && owned(*thread) == &record &&
            process->state() != sched::ProcessState::zombie && process->state() != sched::ProcessState::exited &&
            thread->state != sched::ProcessState::exited && thread->native_active && thread->syscall_pending &&
            process->resources().personality == sched::UserPersonality::linux &&
            same_space(process->resources().native_space, record.space) && same_frame(thread->native_frame, record.frame);
    }
    [[nodiscard]] static bool current_running(sched::SchedulerState &scheduler, const Record &record)
    {
        const auto *thread = scheduler.current_thread();
        return thread && scheduler.current_pid() == record.pid && thread->tid == record.tid &&
            thread->state == sched::ProcessState::running;
    }
    [[nodiscard]] static bool owns_active_wait(sched::SchedulerState &scheduler, const Record &record)
    {
        const auto *thread = scheduler.find_thread(record.pid, record.tid);
        return record.pending && thread && thread->state == sched::ProcessState::blocked &&
            thread->wait_reason == sched::ThreadWaitReason::vfs_operation &&
            thread->wait_key == record.pending->sequence && thread->wait_registration.pending(record.pending->ticket);
    }
    [[nodiscard]] static i64 error(Status status)
    { return status.code() == StatusCode::unsupported ? -95 : -syscall::ErrnoMapper::errno_for(status); }
    static i64 terminal(Record &record, i64 result)
    { record.captured = false; return result; }

    template <typename Admit, typename Copy>
    i64 attempt(sched::SchedulerState &scheduler, Record &record, Admit admit, Copy copy, bool &complete)
    {
        auto &request = *record.pending;
        auto result = request.vfs->try_stat_absolute(request.path.view(), request.credentials,
            (request.operation.flags & posix::at_SYMLINK_NOFOLLOW) ? fs::FinalComponent::no_follow : fs::FinalComponent::follow);
        if (result.contention.valid())
        {
            const auto wait = scheduler.register_current_wait(sched::ThreadWaitReason::vfs_operation, request.sequence);
            if (!wait)
            {
                const auto value = error(wait.status());
                static_cast<void>(cancel(scheduler, {this, record.lease.slot, request.sequence}));
                return value;
            }
            request.ticket = wait.value();
            auto queued = request.vfs->queue_certified_operation_wait(std::move(result.contention), request.ticket, this);
            if (!queued)
            {
                const auto value = error(queued.status());
                static_cast<void>(cancel(scheduler, {this, record.lease.slot, request.sequence}));
                return value;
            }
            if (!queued.value() && !scheduler.complete_wait(request.ticket, sched::WaitCompletion::ready))
            {
                static_cast<void>(cancel(scheduler, {this, record.lease.slot, request.sequence}));
                return -125;
            }
            complete = false;
            return -11; // Internal only; runtime must not publish this result.
        }
        i64 value = result.result ? encode(request, result.result.value(), admit, copy) : error(result.result.status());
        const auto cleanup = cancel(scheduler, {this, record.lease.slot, request.sequence});
        return value == 0 && !cleanup ? error(cleanup) : value;
    }

    template <typename Admit, typename Copy>
    static i64 encode(const Pending &request, const fs::Metadata &metadata, Admit admit, Copy copy)
    {
        const posix::FileStatus value{metadata.type, metadata.size, metadata.mode, metadata.uid, metadata.gid,
            metadata.link_count, metadata.block_size, metadata.blocks, metadata.attributes};
        std::array<u8, 256> bytes{};
        const auto length = abi::linux_user::StatCodec<Selected>::size(request.operation.layout);
        const auto encoded = abi::linux_user::StatCodec<Selected>::encode(value, request.operation.layout,
                                                                        {bytes.data(), length});
        if (!encoded) return error(encoded.status());
        const auto output = request.operation.output;
        if (!output || length - 1 > Codec::word_mask - output || !admit(output, length)) return -14;
        const auto copied = copy(output, std::span<const u8>{bytes.data(), length});
        return copied ? 0 : error(copied);
    }

    Pool *pool_{};
    BackendCapture legacy_capture_;
    std::array<Record, capacity> records_{};
    u64 images_{}, captures_{}, requests_{};
};

} // namespace ok::user::native
