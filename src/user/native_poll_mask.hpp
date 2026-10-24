#pragma once

#include "ok/sched/scheduler.hpp"
#include "ok/memory/object_pool.hpp"
#include "ok/memory/kernel_entry_stack_pool.hpp"

namespace ok::user::native
{
// Private serialized runtime state. No exported PCB/TCB/request layout changes.
class PollSignalMasks
{
public:
    enum class Phase : u8 { none, admitted, waiting, delivery };
    struct Record
    {
        memory::KernelEntryStackHandle lease{};
        sched::ProcessId pid{};
        sched::ThreadId tid{};
        sched::SignalMask original{};
        memory::ObjectHandle object{};
        sched::WaitTicket ticket{};
        Phase phase{};
    };
    [[nodiscard]] Record *find(const sched::ThreadControlBlock &thread)
    {
        if (!thread.entry_stack.pool || thread.entry_stack.slot >= records_.size()) return nullptr;
        auto &r = records_[thread.entry_stack.slot];
        return r.lease == thread.entry_stack && r.pid == thread.owner && r.tid == thread.tid &&
               r.phase != Phase::none ? &r : nullptr;
    }
    [[nodiscard]] bool begin(sched::ThreadControlBlock &thread, sched::SignalMask requested)
    {
        if (!thread.entry_stack.pool || thread.entry_stack.slot >= records_.size() || find(thread)) return false;
        auto &r = records_[thread.entry_stack.slot];
        // An old generation has no authority over the new lease.
        r = {thread.entry_stack, thread.owner, thread.tid, thread.native_signal_mask, {}, {}, Phase::admitted};
        thread.native_signal_mask = requested;
        return true;
    }
    void restore(sched::ThreadControlBlock &thread)
    {
        if (auto *r = find(thread)) { thread.native_signal_mask = r->original; *r = {}; }
    }
    [[nodiscard]] bool bind(sched::ThreadControlBlock &thread, memory::ObjectHandle object, sched::WaitTicket ticket)
    {
        auto *r = find(thread);
        if (!r) return true;
        if (r->phase != Phase::admitted) return false;
        r->object = object; r->ticket = ticket; r->phase = Phase::waiting;
        return true;
    }
    void complete(sched::ThreadControlBlock &thread, memory::ObjectHandle object,
                  sched::WaitTicket ticket, bool interrupted)
    {
        auto *r = find(thread);
        if (!r || r->phase != Phase::waiting || r->object != object || r->ticket != ticket) return;
        if (interrupted) r->phase = Phase::delivery;
        else restore(thread);
    }
    [[nodiscard]] const sched::SignalMask *saved(const sched::ThreadControlBlock &thread)
    {
        auto *r = find(thread);
        return r && r->phase == Phase::delivery ? &r->original : nullptr;
    }
    void handler_committed(const sched::ThreadControlBlock &thread)
    {
        if (auto *r = find(thread); r && r->phase == Phase::delivery) *r = {};
    }
    [[nodiscard]] sched::SignalMask exec_mask(const sched::ThreadControlBlock &thread)
    {
        if (auto *r = find(thread)) return r->original;
        return thread.native_signal_mask;
    }
    void discard_process(sched::ProcessId pid)
    {
        for (auto &r : records_) if (r.pid == pid) r = {};
    }
    [[nodiscard]] const auto &records() const { return records_; }
private:
    std::array<Record, memory::KernelEntryStackPool::capacity> records_{};
};
} // namespace ok::user::native
