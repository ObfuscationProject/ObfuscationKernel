#pragma once

#include "ok/arch/arch.hpp"
#include "ok/smp/cpu.hpp"
#include "ok/smp/smp.hpp"

#include <array>
#include <atomic>
#include <concepts>

namespace ok::smp
{

// A CPU takes ranked locks in strictly increasing rank. The gaps leave room
// for ranks that later subsystems slot between these.
enum class LockRank : u8
{
    big_kernel = 10,
    ofd_position = 14,  // One open description's offset and flags across one I/O.
    fs_provider = 16,   // A filesystem's operation gate (a hold recorded by the gate).
    scheduler = 20,
    run_queue = 30,
    wait_queue = 40,
    ipi = 50,
    tlb = 60,
    arch_spaces = 64,   // A backend's address-space slots, space ids and TLB generations.
    memory = 70,
    irq_router = 80,
    realtime_clock = 82,
    monotonic_clock = 84,
    console = 90,
    leaf = 255,
};

enum class LockViolationKind : u8
{
    order,         // The wanted rank is not above every rank this CPU holds.
    self_deadlock, // This CPU already owns the lock it would wait for.
    not_owner,     // Unlock of a lock this CPU does not own.
    unlock_order,  // Unlock out of LIFO order.
    nesting,       // More holds than the per-CPU record tracks.
};

struct LockViolation
{
    LockViolationKind kind{LockViolationKind::order};
    LockRank held{};
    LockRank wanted{};
    const void *lock{nullptr};
    CpuId cpu{0};
};

using LockViolationHandler = void (*)(const LockViolation &) noexcept;
// Null only counts violations; a diagnostic hook may log or panic instead.
inline constinit LockViolationHandler lock_violation_handler{};

inline constexpr usize lock_debug_capacity = 8;

// Per-CPU kernel mode bits (ok/smp/kernel_section.hpp). Only the owning CPU
// writes its word, with a load and a store; other CPUs read it for evidence.
inline constexpr u32 cpu_mode_bkl_held = 1; // Holds the big kernel lock.
inline constexpr u32 cpu_mode_section = 2;  // Runs a kernel section without it.
inline constexpr u32 cpu_mode_spinning = 4; // Waits for a lock, a gate, a drain or a device.

struct alignas(64) CpuMode
{
    std::atomic<u32> bits{};
    // Times this CPU started a wait (SpinningScope that set the bit).
    std::atomic<uptr> spins{};
};
inline constinit std::array<CpuMode, max_cpus> cpu_modes{};

[[nodiscard]] inline u32 cpu_mode(CpuId cpu) noexcept
{
    return cpu < max_cpus ? cpu_modes[cpu].bits.load(std::memory_order_relaxed) : 0;
}

inline void set_cpu_mode(CpuId cpu, u32 bits) noexcept
{
    if (cpu < max_cpus)
        cpu_modes[cpu].bits.store(cpu_modes[cpu].bits.load(std::memory_order_relaxed) | bits,
                                  std::memory_order_relaxed);
}

inline void clear_cpu_mode(CpuId cpu, u32 bits) noexcept
{
    if (cpu < max_cpus)
        cpu_modes[cpu].bits.store(cpu_modes[cpu].bits.load(std::memory_order_relaxed) & ~bits,
                                  std::memory_order_relaxed);
}

// Marks this CPU SPINNING while it waits; a nested scope leaves the outer
// one's bit alone. Lock waits, gate and drain waits and halts that hold the
// big kernel lock use it, so overlap evidence never counts a waiting CPU.
class SpinningScope final
{
  public:
    SpinningScope() noexcept : cpu_(current_cpu())
    {
        if (cpu_ >= max_cpus || (cpu_mode(cpu_) & cpu_mode_spinning) != 0)
            return;
        auto &spins = cpu_modes[cpu_].spins;
        spins.store(spins.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        set_cpu_mode(cpu_, cpu_mode_spinning);
        set_ = true;
    }
    ~SpinningScope()
    {
        if (set_)
            clear_cpu_mode(cpu_, cpu_mode_spinning);
    }

    SpinningScope(const SpinningScope &) = delete;
    SpinningScope &operator=(const SpinningScope &) = delete;

  private:
    CpuId cpu_;
    bool set_{false};
};

struct LockDebugCpu
{
    u8 depth{0};
    std::array<LockRank, lock_debug_capacity> ranks{};
    std::array<const void *, lock_debug_capacity> locks{};
    // Self-deadlocking lock() calls that returned without the lock; the
    // matching unlock() consumes one instead of releasing the outer hold.
    u8 refused_depth{0};
    std::array<const void *, lock_debug_capacity> refused{};
    u64 violations{0};
};

#if defined(OK_LOCK_DEBUG)
inline constexpr bool lock_debug_enabled = true;
// Each record is touched only by its own CPU, with interrupts masked by the
// caller or by RankedSpinGuard.
inline constinit std::array<LockDebugCpu, max_cpus> lock_debug_cpus{};
#else
inline constexpr bool lock_debug_enabled = false;
#endif

inline void lock_rank_violation(LockViolation violation) noexcept
{
#if defined(OK_LOCK_DEBUG)
    if (violation.cpu < max_cpus)
        ++lock_debug_cpus[violation.cpu].violations;
#endif
    if (const auto handler = lock_violation_handler; handler != nullptr)
        handler(violation);
}

[[nodiscard]] inline u64 lock_violation_count([[maybe_unused]] CpuId cpu) noexcept
{
#if defined(OK_LOCK_DEBUG)
    return cpu < max_cpus ? lock_debug_cpus[cpu].violations : 0;
#else
    return 0;
#endif
}

#if defined(OK_LOCK_DEBUG)
namespace detail
{

[[nodiscard]] inline usize lock_debug_recorded(const LockDebugCpu &record) noexcept
{
    return record.depth < lock_debug_capacity ? record.depth : lock_debug_capacity;
}

// The wanted rank must be above every rank this CPU holds.
inline void lock_debug_check_order(const void *lock, LockRank rank, CpuId cpu) noexcept
{
    if (cpu >= max_cpus)
        return;
    const auto &record = lock_debug_cpus[cpu];
    LockRank highest{};
    bool holds = false;
    for (usize index = 0; index < lock_debug_recorded(record); ++index)
    {
        if (!holds || record.ranks[index] > highest)
            highest = record.ranks[index];
        holds = true;
    }
    if (holds && highest >= rank)
        lock_rank_violation({LockViolationKind::order, highest, rank, lock, cpu});
}

// Checked before waiting, so a self-deadlock is reported instead of spinning.
[[nodiscard]] inline bool lock_debug_may_wait(const void *lock, LockRank rank, CpuId cpu, bool owned) noexcept
{
    if (cpu >= max_cpus)
        return true;
    if (owned)
    {
        lock_rank_violation({LockViolationKind::self_deadlock, rank, rank, lock, cpu});
        auto &record = lock_debug_cpus[cpu];
        if (record.refused_depth < lock_debug_capacity)
            record.refused[record.refused_depth++] = lock;
        return false;
    }
    lock_debug_check_order(lock, rank, cpu);
    return true;
}

// True when this unlock() matches a refused self-deadlocking lock().
[[nodiscard]] inline bool lock_debug_consume_refused(const void *lock, CpuId cpu) noexcept
{
    if (cpu >= max_cpus)
        return false;
    auto &record = lock_debug_cpus[cpu];
    for (usize index = record.refused_depth; index != 0; --index)
        if (record.refused[index - 1] == lock)
        {
            for (usize next = index; next < record.refused_depth; ++next)
                record.refused[next - 1] = record.refused[next];
            --record.refused_depth;
            return true;
        }
    return false;
}

inline void lock_debug_acquired(const void *lock, LockRank rank, CpuId cpu) noexcept
{
    if (cpu >= max_cpus)
        return;
    auto &record = lock_debug_cpus[cpu];
    if (record.depth < lock_debug_capacity)
    {
        record.ranks[record.depth] = rank;
        record.locks[record.depth] = lock;
    }
    else
        lock_rank_violation({LockViolationKind::nesting, record.ranks[lock_debug_capacity - 1], rank, lock, cpu});
    if (record.depth != 0xff)
        ++record.depth;
}

inline void lock_debug_released(const void *lock, LockRank rank, CpuId cpu) noexcept
{
    if (cpu >= max_cpus)
        return;
    auto &record = lock_debug_cpus[cpu];
    // Holds past the record's capacity were reported when taken and are not tracked.
    if (record.depth > lock_debug_capacity)
    {
        --record.depth;
        return;
    }
    const usize top = record.depth;
    usize index = top;
    while (index != 0 && record.locks[index - 1] != lock)
        --index;
    if (index != top)
        lock_rank_violation({LockViolationKind::unlock_order, top != 0 ? record.ranks[top - 1] : rank, rank, lock,
                             cpu});
    if (index == 0)
        return;
    for (usize next = index; next < top; ++next)
    {
        record.ranks[next - 1] = record.ranks[next];
        record.locks[next - 1] = record.locks[next];
    }
    --record.depth;
}

} // namespace detail
#endif

// Holds recorded on `cpu` (0 without OK_LOCK_DEBUG, where nothing is recorded).
[[nodiscard]] inline usize lock_debug_depth([[maybe_unused]] CpuId cpu) noexcept
{
#if defined(OK_LOCK_DEBUG)
    return cpu < max_cpus ? lock_debug_cpus[cpu].depth : 0;
#else
    return 0;
#endif
}

// For a gate that is not a RankedSpinLock (an owner word kept under an
// unranked lock): the same order checks and records as a lock of `rank`.
// Call lock_debug_may_wait before waiting for the gate, lock_debug_hold once
// this CPU owns it and lock_debug_release when it gives it up. Reentry on
// the owning CPU is the gate's to refuse; it takes no hold. Release builds
// compile these to nothing.
inline void lock_debug_may_wait([[maybe_unused]] const void *gate, [[maybe_unused]] LockRank rank) noexcept
{
#if defined(OK_LOCK_DEBUG)
    detail::lock_debug_check_order(gate, rank, current_cpu());
#endif
}

inline void lock_debug_hold([[maybe_unused]] const void *gate, [[maybe_unused]] LockRank rank) noexcept
{
#if defined(OK_LOCK_DEBUG)
    detail::lock_debug_acquired(gate, rank, current_cpu());
#endif
}

inline void lock_debug_release([[maybe_unused]] const void *gate, [[maybe_unused]] LockRank rank) noexcept
{
#if defined(OK_LOCK_DEBUG)
    detail::lock_debug_released(gate, rank, current_cpu());
#endif
}

// Test-and-test-and-set lock that records its owner (CPU + 1, 0 = free). The
// flag needs only an atomic swap, which every profile has (i386 has no
// CMPXCHG); only the holder writes the owner word. With OK_LOCK_DEBUG a rank,
// self-deadlock, owner or LIFO violation is reported to lock_rank_violation;
// a self-deadlocking lock() returns without the lock rather than spin forever.
// Release builds compile the checks out. A waiting CPU is marked SPINNING.
template <LockRank Rank> class RankedSpinLock final
{
  public:
    static constexpr LockRank rank = Rank;

    constexpr RankedSpinLock() noexcept = default;
    RankedSpinLock(const RankedSpinLock &) = delete;
    RankedSpinLock &operator=(const RankedSpinLock &) = delete;

    // True when the lock was held elsewhere and this CPU waited for it.
    bool lock() noexcept
    {
        const auto cpu = current_cpu();
#if defined(OK_LOCK_DEBUG)
        if (!detail::lock_debug_may_wait(this, Rank, cpu,
                                         owner_.load(std::memory_order_relaxed) == owner_value(cpu)))
            return false;
#endif
        bool waited = false;
        if (locked_.test_and_set(std::memory_order_acquire))
        {
            waited = true;
            const SpinningScope spinning;
            do
            {
                while (locked_.test(std::memory_order_relaxed))
                    arch::ArchOperations::spin_pause();
            } while (locked_.test_and_set(std::memory_order_acquire));
        }
        owner_.store(owner_value(cpu), std::memory_order_relaxed);
#if defined(OK_LOCK_DEBUG)
        detail::lock_debug_acquired(this, Rank, cpu);
#endif
        return waited;
    }

    void unlock() noexcept
    {
#if defined(OK_LOCK_DEBUG)
        const auto cpu = current_cpu();
        if (owner_.load(std::memory_order_relaxed) != owner_value(cpu))
        {
            lock_rank_violation({LockViolationKind::not_owner, Rank, Rank, this, cpu});
            return;
        }
        if (detail::lock_debug_consume_refused(this, cpu))
            return;
        detail::lock_debug_released(this, Rank, cpu);
#endif
        owner_.store(0, std::memory_order_relaxed);
        locked_.clear(std::memory_order_release);
    }

    // try_lock cannot deadlock, but a hold it takes out of rank order breaks
    // the order other paths rely on, so a successful one is order checked
    // and recorded. A try on a lock this CPU owns fails without a report.
    [[nodiscard]] bool try_lock() noexcept
    {
        if (locked_.test(std::memory_order_relaxed) || locked_.test_and_set(std::memory_order_acquire))
            return false;
        const auto cpu = current_cpu();
        owner_.store(owner_value(cpu), std::memory_order_relaxed);
#if defined(OK_LOCK_DEBUG)
        detail::lock_debug_check_order(this, Rank, cpu);
        detail::lock_debug_acquired(this, Rank, cpu);
#endif
        return true;
    }

    [[nodiscard]] bool owned_by_current_cpu() const noexcept
    {
        return owner_.load(std::memory_order_relaxed) == owner_value(current_cpu());
    }

    // For callers that already masked interrupts.
    class Guard final
    {
      public:
        explicit Guard(RankedSpinLock &lock) noexcept : lock_(lock)
        {
            lock_.lock();
        }
        ~Guard()
        {
            lock_.unlock();
        }

        Guard(const Guard &) = delete;
        Guard &operator=(const Guard &) = delete;

      private:
        RankedSpinLock &lock_;
    };

  private:
    [[nodiscard]] static constexpr u32 owner_value(CpuId cpu) noexcept
    {
        return static_cast<u32>(cpu) + 1;
    }

    std::atomic_flag locked_{};
    std::atomic<u32> owner_{0};
};

template <typename T>
concept InterruptMask = requires(uptr saved) {
    { T::save_and_disable_interrupts() } noexcept -> std::same_as<uptr>;
    { T::restore_interrupts(saved) } noexcept -> std::same_as<void>;
};

#if defined(OK_KERNEL_FREESTANDING)
using KernelInterruptMask = arch::ArchOperations;
#else
// Hosted model builds have no interrupt state and need no arch library.
struct KernelInterruptMask
{
    [[nodiscard]] static uptr save_and_disable_interrupts() noexcept
    {
        return 0;
    }
    static void restore_interrupts(uptr) noexcept {}
};
#endif
static_assert(InterruptMask<KernelInterruptMask>);

// Masks interrupts before acquiring and restores the saved state after
// releasing, so an interrupt handler on this CPU cannot spin on the lock.
template <LockRank Rank, InterruptMask Interrupts = KernelInterruptMask> class RankedSpinGuard final
{
  public:
    explicit RankedSpinGuard(RankedSpinLock<Rank> &lock) noexcept
        : lock_(lock), saved_(Interrupts::save_and_disable_interrupts())
    {
        lock_.lock();
    }
    ~RankedSpinGuard()
    {
        lock_.unlock();
        Interrupts::restore_interrupts(saved_);
    }

    RankedSpinGuard(const RankedSpinGuard &) = delete;
    RankedSpinGuard &operator=(const RankedSpinGuard &) = delete;

  private:
    RankedSpinLock<Rank> &lock_;
    uptr saved_;
};

} // namespace ok::smp
