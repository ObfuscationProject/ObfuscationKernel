#pragma once

#include "ok/core/types.hpp"
#include "ok/smp/lock.hpp"

#include <atomic>

namespace ok::smp
{

// Class B gates (ok/smp/lock.hpp ranks below first_spin_rank): the holder may
// wait for a device, a user copy, a quiesce or a lower class B gate. The
// pattern of the ext4 operation gate and the description position guard:
//   - re-entry on the owning CPU returns Status::busy and takes nothing;
//   - entering while this CPU holds a class A lock counts class_asserts (I1);
//   - a waiter spins outside any lock with the CPU marked SPINNING and serves
//     its pending interrupts on every pass (serve_while_spinning), since the
//     holder may wait for a device line routed to the waiting CPU;
//   - the hold is order-checked and recorded at the gate's rank like a lock.
// Only an atomic flag swap and plain loads and stores are used, so every
// profile has them (i386 has no compare-exchange).

namespace detail
{
[[nodiscard]] constexpr u32 gate_owner(CpuId cpu) noexcept
{
    return static_cast<u32>(cpu) + 1;
}

// Spins while `busy` holds, serving this CPU's interrupts.
template <typename Busy> inline void gate_wait(Busy busy) noexcept
{
    while (busy())
    {
        serve_while_spinning();
        arch::ArchOperations::spin_pause();
    }
}
} // namespace detail

// One holder at a time.
template <LockRank Rank> class ServingGate final
{
    static_assert(lock_class(Rank) == LockClass::serving, "a gate is a class B rank");

  public:
    static constexpr LockRank rank = Rank;

    constexpr ServingGate() noexcept = default;
    ServingGate(const ServingGate &) = delete;
    ServingGate &operator=(const ServingGate &) = delete;

    [[nodiscard]] Status enter() noexcept
    {
        const auto cpu = current_cpu();
        if (owner_.load(std::memory_order_relaxed) == detail::gate_owner(cpu))
            return Status::busy("gate re-entered on its owning CPU");
        static_cast<void>(check_class_wait("gate entered under a spin lock"));
        lock_debug_may_wait(this, Rank);
        if (!take(cpu))
        {
            count_lock_contended(Rank, cpu);
            const SpinningScope spinning;
            do
                detail::gate_wait([this] { return taken_.test(std::memory_order_relaxed); });
            while (!take(cpu));
        }
        lock_debug_hold(this, Rank);
        return {};
    }

    // Never waits; false when the gate is held, here or elsewhere.
    [[nodiscard]] bool try_enter() noexcept
    {
        const auto cpu = current_cpu();
        if (owner_.load(std::memory_order_relaxed) == detail::gate_owner(cpu) || !take(cpu))
            return false;
        lock_debug_may_wait(this, Rank);
        lock_debug_hold(this, Rank);
        return true;
    }

    void leave() noexcept
    {
        const auto cpu = current_cpu();
        if (owner_.load(std::memory_order_relaxed) != detail::gate_owner(cpu))
        {
            if constexpr (lock_debug_enabled)
                lock_rank_violation({LockViolationKind::not_owner, Rank, Rank, this, cpu});
            return;
        }
        lock_debug_release(this, Rank);
        owner_.store(0, std::memory_order_relaxed);
        taken_.clear(std::memory_order_release);
    }

    [[nodiscard]] bool owned_by_current_cpu() const noexcept
    {
        return owner_.load(std::memory_order_relaxed) == detail::gate_owner(current_cpu());
    }

    [[nodiscard]] bool held() const noexcept
    {
        return taken_.test(std::memory_order_relaxed);
    }

    // The holding CPU + 1, 0 when free (evidence and tests; it may change at once).
    [[nodiscard]] u32 owner() const noexcept
    {
        return owner_.load(std::memory_order_relaxed);
    }

    // Holds the gate for a scope when enter() succeeded; status() says why not.
    class Guard final
    {
      public:
        explicit Guard(ServingGate &gate) noexcept : gate_(gate), status_(gate.enter()) {}
        ~Guard()
        {
            if (status_)
                gate_.leave();
        }
        Guard(const Guard &) = delete;
        Guard &operator=(const Guard &) = delete;

        [[nodiscard]] const Status &status() const noexcept
        {
            return status_;
        }

      private:
        ServingGate &gate_;
        Status status_;
    };

  private:
    [[nodiscard]] bool take(CpuId cpu) noexcept
    {
        if (taken_.test(std::memory_order_relaxed) || taken_.test_and_set(std::memory_order_acquire))
            return false;
        owner_.store(detail::gate_owner(cpu), std::memory_order_relaxed);
        return true;
    }

    std::atomic_flag taken_{};
    std::atomic<u32> owner_{0};
};

// Many shared holders or one exclusive holder (the VFS namespace: lookups
// shared, mount and detach exclusive). A waiting exclusive entry holds new
// shared entries off, so a stream of readers cannot starve it. The state
// words are changed only under `state_`, an unranked flag held for a few
// loads and stores, and read lock-free by waiters.
template <LockRank Rank> class SharedGate final
{
    static_assert(lock_class(Rank) == LockClass::serving, "a gate is a class B rank");
    static_assert(max_cpus <= 32, "shared holders are a CPU mask");

  public:
    static constexpr LockRank rank = Rank;

    constexpr SharedGate() noexcept = default;
    SharedGate(const SharedGate &) = delete;
    SharedGate &operator=(const SharedGate &) = delete;

    [[nodiscard]] Status enter_shared() noexcept
    {
        const auto cpu = current_cpu();
        if (owned_by(cpu))
            return Status::busy("shared gate re-entered on a holding CPU");
        static_cast<void>(check_class_wait("shared gate entered under a spin lock"));
        lock_debug_may_wait(this, Rank);
        if (!take_shared(cpu))
        {
            count_lock_contended(Rank, cpu);
            const SpinningScope spinning;
            do
                detail::gate_wait([this] {
                    return writer_.load(std::memory_order_relaxed) != 0 ||
                           waiting_writer_.load(std::memory_order_relaxed) != 0;
                });
            while (!take_shared(cpu));
        }
        lock_debug_hold(this, Rank);
        return {};
    }

    void leave_shared() noexcept
    {
        const auto cpu = current_cpu();
        if ((readers_.load(std::memory_order_relaxed) & bit(cpu)) == 0)
        {
            if constexpr (lock_debug_enabled)
                lock_rank_violation({LockViolationKind::not_owner, Rank, Rank, this, cpu});
            return;
        }
        lock_debug_release(this, Rank);
        const StateGuard guard(*this);
        readers_.store(readers_.load(std::memory_order_relaxed) & ~bit(cpu), std::memory_order_relaxed);
    }

    [[nodiscard]] Status enter_exclusive() noexcept
    {
        const auto cpu = current_cpu();
        if (owned_by(cpu))
            return Status::busy("shared gate re-entered on a holding CPU");
        static_cast<void>(check_class_wait("shared gate entered under a spin lock"));
        lock_debug_may_wait(this, Rank);
        if (!take_exclusive(cpu, true))
        {
            count_lock_contended(Rank, cpu);
            const SpinningScope spinning;
            do
                detail::gate_wait([this] {
                    return writer_.load(std::memory_order_relaxed) != 0 ||
                           readers_.load(std::memory_order_relaxed) != 0;
                });
            while (!take_exclusive(cpu, true));
        }
        lock_debug_hold(this, Rank);
        return {};
    }

    void leave_exclusive() noexcept
    {
        const auto cpu = current_cpu();
        if (writer_.load(std::memory_order_relaxed) != detail::gate_owner(cpu))
        {
            if constexpr (lock_debug_enabled)
                lock_rank_violation({LockViolationKind::not_owner, Rank, Rank, this, cpu});
            return;
        }
        lock_debug_release(this, Rank);
        const StateGuard guard(*this);
        writer_.store(0, std::memory_order_relaxed);
    }

    // Never wait; false when the mode is not available now.
    [[nodiscard]] bool try_enter_shared() noexcept
    {
        const auto cpu = current_cpu();
        if (owned_by(cpu) || !take_shared(cpu))
            return false;
        lock_debug_may_wait(this, Rank);
        lock_debug_hold(this, Rank);
        return true;
    }

    [[nodiscard]] bool try_enter_exclusive() noexcept
    {
        const auto cpu = current_cpu();
        if (owned_by(cpu) || !take_exclusive(cpu, false))
            return false;
        lock_debug_may_wait(this, Rank);
        lock_debug_hold(this, Rank);
        return true;
    }

    // Held by this CPU, shared or exclusive.
    [[nodiscard]] bool owned_by_current_cpu() const noexcept
    {
        return owned_by(current_cpu());
    }

    [[nodiscard]] bool owned_exclusive_by_current_cpu() const noexcept
    {
        return writer_.load(std::memory_order_relaxed) == detail::gate_owner(current_cpu());
    }

    // CPUs holding it shared, as a mask.
    [[nodiscard]] u32 shared_holders() const noexcept
    {
        return readers_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] bool exclusive_held() const noexcept
    {
        return writer_.load(std::memory_order_relaxed) != 0;
    }

  private:
    class StateGuard final
    {
      public:
        explicit StateGuard(SharedGate &gate) noexcept : gate_(gate)
        {
            while (gate_.state_.test_and_set(std::memory_order_acquire))
                while (gate_.state_.test(std::memory_order_relaxed))
                    arch::ArchOperations::spin_pause();
        }
        ~StateGuard()
        {
            gate_.state_.clear(std::memory_order_release);
        }
        StateGuard(const StateGuard &) = delete;
        StateGuard &operator=(const StateGuard &) = delete;

      private:
        SharedGate &gate_;
    };

    [[nodiscard]] static constexpr u32 bit(CpuId cpu) noexcept
    {
        return u32{1} << cpu;
    }

    [[nodiscard]] bool owned_by(CpuId cpu) const noexcept
    {
        return (readers_.load(std::memory_order_relaxed) & bit(cpu)) != 0 ||
               writer_.load(std::memory_order_relaxed) == detail::gate_owner(cpu);
    }

    [[nodiscard]] bool take_shared(CpuId cpu) noexcept
    {
        const StateGuard guard(*this);
        if (writer_.load(std::memory_order_relaxed) != 0 || waiting_writer_.load(std::memory_order_relaxed) != 0)
            return false;
        readers_.store(readers_.load(std::memory_order_relaxed) | bit(cpu), std::memory_order_relaxed);
        return true;
    }

    // `announce`: a failed attempt marks this CPU as the waiting writer when
    // none is, holding new shared entries off until a writer gets in.
    [[nodiscard]] bool take_exclusive(CpuId cpu, bool announce) noexcept
    {
        const auto self = detail::gate_owner(cpu);
        const StateGuard guard(*this);
        const auto waiting = waiting_writer_.load(std::memory_order_relaxed);
        if (writer_.load(std::memory_order_relaxed) == 0 && readers_.load(std::memory_order_relaxed) == 0)
        {
            writer_.store(self, std::memory_order_relaxed);
            if (waiting == self)
                waiting_writer_.store(0, std::memory_order_relaxed);
            return true;
        }
        if (announce && waiting == 0)
            waiting_writer_.store(self, std::memory_order_relaxed);
        return false;
    }

    std::atomic_flag state_{};
    std::atomic<u32> readers_{0};
    std::atomic<u32> writer_{0};
    std::atomic<u32> waiting_writer_{0};
};

} // namespace ok::smp
