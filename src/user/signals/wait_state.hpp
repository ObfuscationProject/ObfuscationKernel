#pragma once

#include "wait_codec.hpp"
#include "ok/abi/linux_sleep.hpp"
#include "ok/sched/scheduler.hpp"
#include "ok/memory/kernel_entry_stack_pool.hpp"

namespace ok::user::native
{
// Serialized boot-CPU ownership, separate from poll's saved frame mask.
// Records never borrow PCB/TCB pointers or signal tickets across a step.
template <abi::linux_user::Profile Profile>
class SignalWaitState
{
public:
    using Codec = SignalWaitCodec<Profile>;
    enum class Phase : u8 { none, admitted, waiting, delivery };
    struct Record
    {
        memory::KernelEntryStackHandle lease{};
        sched::ProcessId pid{};
        sched::ThreadId tid{};
        u64 epoch{};
        typename Codec::Request request{};
        sched::SignalMask original{};
        sched::WaitTicket ticket{};
        sched::DeadlineWait timeout{};
        u64 syscall{};
        uptr resume{};
        u64 set_address{}, timeout_address{}, sigsetsize{};
        bool result_staged{};
        Phase phase{};
    };
    [[nodiscard]] Record *find(const sched::ThreadControlBlock &thread)
    {
        return const_cast<Record *>(static_cast<const SignalWaitState *>(this)->find(thread));
    }
    [[nodiscard]] const Record *find(const sched::ThreadControlBlock &thread) const
    {
        if (!thread.entry_stack.pool || thread.entry_stack.slot >= records_.size()) return nullptr;
        const auto &r = records_[thread.entry_stack.slot];
        return r.phase != Phase::none && r.lease == thread.entry_stack &&
               r.pid == thread.owner && r.tid == thread.tid ? &r : nullptr;
    }
    [[nodiscard]] bool begin(sched::ThreadControlBlock &thread, const typename Codec::Request &request,
                             u64 syscall, u64 now, std::array<u64, 3> input = {})
    {
        if (!thread.entry_stack.pool || thread.entry_stack.slot >= records_.size() || find(thread)) return false;
        auto &epoch = epochs_[thread.entry_stack.slot];
        if (epoch == ~u64{0}) return false;
        Record next{};
        next.lease = thread.entry_stack; next.pid = thread.owner; next.tid = thread.tid;
        next.epoch = ++epoch; next.request = request; next.original = thread.native_signal_mask;
        next.syscall = syscall; next.resume = thread.native_frame.pc; next.phase = Phase::admitted;
        next.set_address = input[0]; next.timeout_address = input[1]; next.sigsetsize = input[2];
        if (request.kind == SignalWaitKind::timed && request.has_timeout)
        {
            const auto deadline = abi::linux_user::linux_sleep_deadline(now, request.timeout, false);
            if (!next.timeout.arm(0, static_cast<i64>(deadline / 1000000000),
                                   static_cast<i64>(deadline % 1000000000), true)) return false;
        }
        records_[thread.entry_stack.slot] = next;
        if (request.kind == SignalWaitKind::suspend) thread.native_signal_mask = request.wanted;
        else
            for (usize i = 0; i < thread.native_signal_mask.words.size(); ++i)
                thread.native_signal_mask.words[i] &= ~request.wanted.words[i];
        return true;
    }
    [[nodiscard]] bool bind(sched::ThreadControlBlock &thread, sched::WaitTicket ticket)
    {
        auto *r = find(thread);
        if (!r || r->phase != Phase::admitted || !thread.wait_registration.pending(ticket) ||
            ticket.owner != r->pid || ticket.thread != r->tid) return false;
        r->ticket = ticket; r->phase = Phase::waiting; return true;
    }
    [[nodiscard]] const sched::SignalMask *real_blocked(const sched::ThreadControlBlock &thread) const
    {
        const auto *r = find(thread);
        if (!r || r->phase != Phase::waiting || r->request.kind != SignalWaitKind::timed ||
            !thread.native_active || !thread.syscall_pending || thread.wait_registration.ticket() != r->ticket)
            return nullptr;
        if (thread.wait_registration.pending(r->ticket))
            return thread.state == sched::ProcessState::blocked &&
                thread.wait_reason == sched::ThreadWaitReason::signal_wait && thread.wait_key == r->epoch
                ? &r->original : nullptr;
        // Scheduler wakeup precedes recipient-side service. The logical wait
        // still owns its original mask until completion or cancellation restores
        // it; do not drop ignored generation in that runnable interval.
        const auto completion = thread.wait_registration.completion();
        return !thread.wait_registration.active() &&
            (thread.state == sched::ProcessState::runnable || thread.state == sched::ProcessState::running) &&
            thread.wait_reason == sched::ThreadWaitReason::none && thread.wait_key == 0 &&
            (completion == sched::WaitCompletion::ready || completion == sched::WaitCompletion::cancelled ||
             completion == sched::WaitCompletion::interrupted || completion == sched::WaitCompletion::timeout)
            ? &r->original : nullptr;
    }
    [[nodiscard]] const sched::SignalMask *saved(const sched::ThreadControlBlock &thread) const
    {
        const auto *r = find(thread);
        return r && r->phase == Phase::delivery && r->request.kind == SignalWaitKind::suspend ? &r->original : nullptr;
    }
    void handler_committed(const sched::ThreadControlBlock &thread)
    {
        if (auto *r = find(thread); r && r->phase == Phase::delivery) *r = {};
    }
    void restore(sched::ThreadControlBlock &thread)
    {
        if (auto *r = find(thread))
        {
            thread.native_signal_mask = r->original;
            if (thread.wait_registration.pending(r->ticket)) thread.wait_registration.cancel();
            *r = {};
        }
    }
    [[nodiscard]] sched::SignalMask exec_mask(const sched::ThreadControlBlock &thread) const
    {
        const auto *r = find(thread); return r ? r->original : thread.native_signal_mask;
    }
    void discard_process(sched::ProcessId pid)
    {
        for (auto &r : records_) if (r.pid == pid) r = {};
    }
    [[nodiscard]] const auto &records() const { return records_; }
private:
    std::array<Record, memory::KernelEntryStackPool::capacity> records_{};
    std::array<u64, memory::KernelEntryStackPool::capacity> epochs_{};
};
} // namespace ok::user::native
