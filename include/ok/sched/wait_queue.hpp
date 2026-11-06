#pragma once

#include "ok/core/concepts.hpp"
#include "ok/core/types.hpp"
#include "ok/sched/wait_registration.hpp"

#include <array>
#include <concepts>

namespace ok::sched
{

// Bounded queue of armed WaitRegistration records attached to one wait
// condition (a pipe, a device completion, a socket). Protocol, in order:
//   1. ticket = queue.prepare(registration)   -- register the waiter
//   2. recheck the condition                  -- if ready: queue.cancel(ticket), do not sleep
//   3. sleep until registration.completion() != none
// Wakers call wake_one/wake_all after publishing the state change. A waker may
// run from an interrupt top half: the queue does no allocation, no blocking and
// no callback. Serialised caller for now; the IRQ-safe lock arrives with the
// irq-wait-time worktree's lock ranks and SMP with feat/smp-bringup.
//
// A registration the scheduler armed is enlisted instead of prepared; one
// registration may be enlisted in several queues (a thread waiting on several
// sockets). Each entry keeps the ticket it was enlisted with: once any queue or
// the scheduler completes that wait, the remaining entries are stale and are
// dropped, never applied to a later wait of the same thread. notify() lets the
// condition's owner decide per waiter, e.g. complete it through the scheduler.
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
        if (!attach(registration, ticket.value()))
        {
            registration.cancel();
            return Status{StatusCode::overflow, "wait queue slots inconsistent"};
        }
        return ticket;
    }

    Status enlist(WaitRegistration &registration)
    {
        if (!registration.active())
            return Status{StatusCode::invalid_argument, "only an armed registration can be enlisted"};
        for (const auto &slot : slots_)
            if (slot.registration == &registration && slot.ticket == registration.ticket())
                return Status{StatusCode::already_exists, "registration is already enlisted"};
        prune();
        if (count_ == Capacity)
            return Status{StatusCode::overflow, "wait queue capacity exhausted"};
        return attach(registration, registration.ticket()) ? Status{}
            : Status{StatusCode::overflow, "wait queue slots inconsistent"};
    }

    bool cancel(WaitTicket ticket)
    {
        for (usize i = 0; i < Capacity; ++i)
        {
            if (live(i) && slots_[i].ticket == ticket)
            {
                slots_[i].registration->complete(ticket, WaitCompletion::cancelled);
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
            if (slots_[i].registration == nullptr)
                continue;
            if (!live(i)) { detach(i); continue; }
            slots_[i].registration->complete(slots_[i].ticket, completion, source);
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

    // Offers each live waiter, oldest first, to `visit(ticket)`, which returns
    // true when it completed (or abandoned) the waiter and false to keep it
    // queued. Stale entries are dropped. Returns the number removed.
    template <class Visit>
        requires requires(Visit &visit, WaitTicket ticket) { { visit(ticket) } -> std::same_as<bool>; }
    usize notify(Visit &&visit)
    {
        usize removed = 0;
        for (usize scanned = 0; scanned < Capacity; ++scanned)
        {
            const usize i = (oldest_ + scanned) % Capacity;
            if (slots_[i].registration == nullptr)
                continue;
            if (!live(i)) { detach(i); continue; }
            const auto ticket = slots_[i].ticket;
            if (!visit(ticket)) continue;
            detach(i); ++removed;
        }
        return removed;
    }

    // Drops entries whose wait already completed elsewhere.
    usize prune()
    {
        usize dropped = 0;
        for (usize i = 0; i < Capacity; ++i)
            if (slots_[i].registration != nullptr && !live(i)) { detach(i); ++dropped; }
        return dropped;
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
    struct Slot
    {
        WaitRegistration *registration{};
        WaitTicket ticket{};
    };
    bool live(usize index) const
    {
        return slots_[index].registration != nullptr && slots_[index].registration->pending(slots_[index].ticket);
    }
    bool attach(WaitRegistration &registration, WaitTicket ticket)
    {
        for (auto &slot : slots_)
            if (slot.registration == nullptr)
            {
                slot = {&registration, ticket};
                ++count_;
                return true;
            }
        return false;
    }
    void detach(usize index)
    {
        slots_[index] = {};
        --count_;
    }
    std::array<Slot, Capacity> slots_{};
    std::array<usize, wake_source_count> wakes_{};
    usize count_{0};
    usize oldest_{0};
};

} // namespace ok::sched
