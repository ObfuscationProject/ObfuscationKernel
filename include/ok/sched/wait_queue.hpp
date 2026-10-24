#pragma once

#include "ok/core/concepts.hpp"
#include "ok/core/types.hpp"
#include "ok/sched/wait_registration.hpp"

#include <array>

namespace ok::sched
{

enum class WakeSource : u8
{
    thread,
    timer,
    interrupt,
};

// Bounded queue of armed WaitRegistration records attached to one wait
// condition (a pipe, a device completion, a socket). Protocol, in order:
//   1. ticket = queue.prepare(registration)   -- register the waiter
//   2. recheck the condition                  -- if ready: queue.cancel(ticket), do not sleep
//   3. sleep until registration.completion() != none
// Wakers call wake_one/wake_all after publishing the state change. A waker may
// run from an interrupt top half: the queue does no allocation, no blocking and
// no callback. Serialised caller for now; the IRQ-safe lock arrives with the
// irq-wait-time worktree's lock ranks and SMP with feat/smp-bringup.
template <usize Capacity> class WaitQueue final : private NonCopyable
{
  public:
    Result<WaitTicket> prepare(WaitRegistration &registration, u64 owner, u64 thread)
    {
        if (count_ == Capacity)
            return Status{StatusCode::overflow, "wait queue capacity exhausted"};
        auto ticket = registration.arm(owner, thread);
        if (!ticket)
            return ticket.status();
        for (usize i = 0; i < Capacity; ++i)
        {
            if (slots_[i] == nullptr)
            {
                slots_[i] = &registration;
                ++count_;
                return ticket;
            }
        }
        registration.cancel();
        return Status{StatusCode::overflow, "wait queue slots inconsistent"};
    }

    bool cancel(WaitTicket ticket)
    {
        for (usize i = 0; i < Capacity; ++i)
        {
            if (slots_[i] != nullptr && slots_[i]->pending(ticket))
            {
                slots_[i]->complete(ticket, WaitCompletion::cancelled);
                detach(i);
                return true;
            }
        }
        return false;
    }

    // Wakes the oldest armed waiter. Returns false when nobody was waiting.
    bool wake_one(WakeSource source, WaitCompletion completion = WaitCompletion::ready)
    {
        for (usize scanned = 0; scanned < Capacity; ++scanned)
        {
            const usize i = (oldest_ + scanned) % Capacity;
            if (slots_[i] == nullptr)
                continue;
            slots_[i]->complete(slots_[i]->ticket(), completion);
            detach(i);
            oldest_ = (i + 1) % Capacity;
            ++wakes_[static_cast<usize>(source)];
            return true;
        }
        return false;
    }

    usize wake_all(WakeSource source, WaitCompletion completion = WaitCompletion::ready)
    {
        usize woken = 0;
        while (wake_one(source, completion))
            ++woken;
        return woken;
    }

    [[nodiscard]] usize waiting() const
    {
        return count_;
    }
    [[nodiscard]] usize wakes(WakeSource source) const
    {
        return wakes_[static_cast<usize>(source)];
    }

  private:
    void detach(usize index)
    {
        slots_[index] = nullptr;
        --count_;
    }
    std::array<WaitRegistration *, Capacity> slots_{};
    std::array<usize, 3> wakes_{};
    usize count_{0};
    usize oldest_{0};
};

} // namespace ok::sched
