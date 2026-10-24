// Included in the serialized native runtime after its continuation globals.
class VfsImageRetirement final : private NonCopyable
{
  public:
    Status prepare(sched::SchedulerState &scheduler, sched::ProcessControlBlock &process)
    {
        if (staged_ || scheduler.find(process.pid()) != &process || process.threads().empty())
            return Status::denied("VFS image retirement has no canonical source image");
        staged_ = true;
        process_ = &process; scheduler_ = &scheduler; pid_ = process.pid();
        image_ = process.address_space_id(); space_ = process.resources().native_space;
        usize requests{};
        for (const auto &thread : process.threads())
        {
            auto handle = vfs_continuations.pending(pid_, thread.tid);
            const auto ticket = vfs_continuations.ticket(handle);
            if (handle.request)
            {
                ++requests;
                const bool active = thread.state == sched::ProcessState::blocked &&
                    thread.wait_reason == sched::ThreadWaitReason::vfs_operation && thread.wait_key == handle.request &&
                    thread.wait_registration.pending(ticket);
                const bool ready = !thread.wait_registration.active() && thread.wait_registration.ticket() == ticket &&
                    thread.wait_registration.completion() == sched::WaitCompletion::ready &&
                    thread.wait_reason == sched::ThreadWaitReason::none && thread.wait_key == 0 &&
                    (thread.state == sched::ProcessState::runnable || thread.state == sched::ProcessState::running);
                if (!thread.entry_stack.pool || scheduler.find_thread(pid_, thread.tid) != &thread || (!active && !ready))
                    return Status::fault("VFS image retirement lost its exact old scheduler ticket");
            }
            records_[count_++] = {thread.tid, thread.entry_stack, handle, ticket, thread.wait_registration.completion()};
        }
        if (vfs_continuations.pending_count(pid_) != requests)
            return Status::fault("VFS image retirement found a request without its canonical old thread");
        requests_ = requests;
        prepared_ = true;
        return {};
    }

    Status consume(sched::SchedulerState &scheduler, sched::ProcessControlBlock &process)
    {
        if (!prepared_ || consumed_ || scheduler_ != &scheduler || process_ != &process ||
            scheduler.find(pid_) != &process || process.pid() != pid_ || process.address_space_id() == image_ ||
            !same_space(process.resources().native_space, space_) || process.threads().size() != 1)
            return Status::fault("VFS image cancellation requires its committed scheduler image");
        if (vfs_continuations.pending_count(pid_) != requests_)
            return Status::fault("VFS image cancellation changed its staged request count");
        const auto &leader = process.threads()[0];
        const bool leader_lease = records_[0].lease.pool ? leader.entry_stack == records_[0].lease
            : !records_[0].handle.request && entry_stacks.validate_inactive(leader.entry_stack, {pid_, records_[0].tid}).ok();
        if (leader.tid != records_[0].tid || !leader_lease ||
            leader.state != sched::ProcessState::runnable || leader.wait_registration.active() ||
            leader.wait_reason != sched::ThreadWaitReason::none || leader.wait_key)
            return Status::fault("VFS image cancellation found a replacement live wait or lease");
        // Validate the entire plan before cancelling any request. Missing old
        // peers are valid only at this committed scheduler collapse boundary.
        for (usize i = 0; i < count_; ++i)
        {
            const auto &old = records_[i];
            if (i && scheduler.find_thread(pid_, old.tid))
                return Status::fault("VFS image cancellation retained an unexpected old peer");
            if (old.lease.pool)
                if (auto status = entry_stacks.validate_inactive(old.lease, {pid_, old.tid}); !status) return status;
            if (vfs_continuations.pending(pid_, old.tid) != old.handle ||
                vfs_continuations.ticket(old.handle) != old.ticket)
                return Status::fault("VFS image cancellation changed its pending request or ticket");
            if (!i && old.handle.request)
            {
                const auto expected = old.completion == sched::WaitCompletion::ready
                    ? sched::WaitCompletion::ready : sched::WaitCompletion::cancelled;
                if (leader.wait_registration.ticket() != old.ticket || leader.wait_registration.completion() != expected)
                    return Status::fault("VFS image cancellation lost its exact committed leader ticket");
            }
        }
        for (usize i = 0; i < count_; ++i)
            if (records_[i].handle.request)
                if (auto status = vfs_continuations.cancel(scheduler, records_[i].handle); !status) return status;
        consumed_ = true;
        return {};
    }

  private:
    using Continuation = VfsStatContinuation<lu::configured_native_profile()>;
    struct Record
    {
        sched::ThreadId tid{};
        memory::KernelEntryStackHandle lease{};
        Continuation::Handle handle{};
        sched::WaitTicket ticket{};
        sched::WaitCompletion completion{};
    };
    static bool same_space(hw::AddressSpace a, hw::AddressSpace b)
    { return a.root == b.root && a.slot == b.slot && a.generation == b.generation; }
    std::array<Record, sched::max_threads_per_process> records_{};
    sched::SchedulerState *scheduler_{};
    sched::ProcessControlBlock *process_{};
    sched::ProcessId pid_{};
    u64 image_{};
    hw::AddressSpace space_{};
    usize count_{}, requests_{};
    bool staged_{}, prepared_{}, consumed_{};
};
