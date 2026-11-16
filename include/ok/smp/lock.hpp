#pragma once

#include "ok/arch/arch.hpp"
#include "ok/smp/cpu.hpp"
#include "ok/smp/smp.hpp"

#include <array>
#include <atomic>
#include <concepts>
#include <string_view>
#include <type_traits>

namespace ok::smp
{

// A CPU takes ranked locks in strictly increasing rank (the global lock
// order). Class B ranks (below `files`) are serving locks and gates whose
// holder may wait: for a device, a user copy, a quiesce or a lower class B
// gate. Class A ranks are spin locks whose holder only touches memory for a
// bounded time. Every class B rank is below every class A rank, so taking a
// class B lock while holding a class A one is an order violation as well.
enum class LockRank : u8
{
    big_kernel = 10,
    vfs_namespace = 14, // Mount table and mount crossing (shared gate).
    ofd_position = 16,  // One open description's offset and flags across one I/O.
    fs_provider = 18,   // A filesystem's operation gate (a hold recorded by the gate).
    block_request = 20, // One in-flight block device request (OS drivers).
    mm = 22,            // One process's address space records; held across quiesce_root.
    files = 30,         // One process's descriptor table.
    ofd_table = 32,
    pipe = 34,
    tty = 36,
    net_stack = 38,
    ram_topology = 39,
    page_cache = 40,
    futex_bucket = 42,
    futex_bucket_second = 43, // The higher bucket of a pair, through lock_as.
    task_table = 46,
    device = 48, // A driver's rings; the lowest rank a bottom half takes.
    wait_hash = 50,
    task = 52,
    deadline = 54,
    run_queue = 56,
    ipi = 58,
    entry_stack = 60,
    mm_retire = 62,
    tlb = 63,
    arch_spaces = 64, // A backend's address-space slots, space ids and TLB generations.
    shared_files = 66,
    object_pool = 68,
    page_pool = 70,
    slab = 72,
    frame = 74,
    irq_router = 80, // The lowest rank a top half takes.
    realtime_clock = 82,
    monotonic_clock = 84,
    hotplug_request = 88, // Leaf.
    console = 90,
    runtime_status = 92, // Leaf: the runtime's failure record.
    driver_abi = 94,     // Leaf: driver ABI wait queue and slab internals.
};

enum class LockClass : u8
{
    serving, // Class B: the holder may wait; waiters serve their interrupts.
    spin,    // Class A: the holder never waits.
};

inline constexpr LockRank first_spin_rank = LockRank::files;
// Top halves take only ranks from irq_router up; bottom halves from device up.
inline constexpr LockRank top_half_floor = LockRank::irq_router;
inline constexpr LockRank bottom_half_floor = LockRank::device;

[[nodiscard]] constexpr LockClass lock_class(LockRank rank) noexcept
{
    return rank < first_spin_rank ? LockClass::serving : LockClass::spin;
}

// Hosted race tests define OK_HOSTED_LOCK_MODEL so that FineLock is the real
// spin lock; profiles that start secondary CPUs need it as well. A UP
// profile's FineLock is an UpLock.
#if defined(OK_ARCH_SMP_START) || defined(OK_HOSTED_LOCK_MODEL)
inline constexpr bool concurrent_locks = true;
#else
inline constexpr bool concurrent_locks = false;
#endif

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

inline constexpr usize lock_debug_capacity = 12;

// Per-CPU kernel mode bits (ok/smp/kernel_section.hpp). Only the owning CPU
// writes its word, with a load and a store; other CPUs read it for evidence.
inline constexpr u32 cpu_mode_bkl_held = 1; // Holds the big kernel lock.
inline constexpr u32 cpu_mode_section = 2;  // Runs a kernel section without it.
inline constexpr u32 cpu_mode_spinning = 4; // Waits for a lock, a gate, a drain or a device.
// Its kernel-lock stretch serves a thread, a device or a section result (not
// an idle scan); set by bkl_mark_work, cleared at the release.
inline constexpr u32 cpu_mode_kernel_work = 8;
// Serves step or syscall work without the big kernel lock.
inline constexpr u32 cpu_mode_kernel_free = 16;
// Runs an interrupt top half (TopHalfScope).
inline constexpr u32 cpu_mode_top_half = 32;
// Walks user translations inside a kernel window (ok/smp/bringup.hpp), where
// it may take no lock outside a top half (W1).
inline constexpr u32 cpu_mode_window = 64;

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

// A wait that spins with interrupts masked and holds nothing an interrupt's
// top half takes (a filesystem gate, a kernel section's device poll) serves
// this CPU's pending interrupts meanwhile, so a holder waiting for a device
// line routed here is not stalled behind the spin. Installed once by the
// runtime where device lines reach secondary CPUs; unset, it does nothing.
using SpinService = void (*)();
inline constinit SpinService spin_service{};

inline void serve_while_spinning() noexcept
{
    if (const auto service = spin_service)
        service();
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

// Marks this CPU TOP_HALF while it runs an interrupt top half; a nested scope
// leaves the outer one's bit alone. Debug builds count a lock below
// top_half_floor taken meanwhile (top_half_asserts).
class TopHalfScope final
{
  public:
    TopHalfScope() noexcept : cpu_(current_cpu())
    {
        if (cpu_ >= max_cpus || (cpu_mode(cpu_) & cpu_mode_top_half) != 0)
            return;
        set_cpu_mode(cpu_, cpu_mode_top_half);
        set_ = true;
    }
    ~TopHalfScope()
    {
        if (set_)
            clear_cpu_mode(cpu_, cpu_mode_top_half);
    }

    TopHalfScope(const TopHalfScope &) = delete;
    TopHalfScope &operator=(const TopHalfScope &) = delete;

  private:
    CpuId cpu_;
    bool set_{false};
};

namespace detail
{
// Counters written only by their own CPU: a load and a store, never a
// read-modify-write atomic (32-bit profiles have no 64-bit one, i386 no
// compare-exchange).
inline void owner_add(std::atomic<uptr> &counter, uptr amount = 1) noexcept
{
    counter.store(counter.load(std::memory_order_relaxed) + amount, std::memory_order_relaxed);
}
} // namespace detail

// Lock classes the OK_SMP_LOCKS evidence line reports, in report order.
enum class LockStat : u8
{
    task_table,
    task,
    run_queue,
    wait_hash,
    futex,
    ofd_table,
    files,
    pipe,
    tty,
    net_stack,
    mm,
    vfs_namespace,
    ofd_position,
    fs_provider,
    ram_topology,
    page_pool,
    frame,
    count,
    none = count,
};
inline constexpr usize lock_stat_count = static_cast<usize>(LockStat::count);
inline constexpr std::array<std::string_view, lock_stat_count> lock_stat_names{
    "task_table", "task",          "run_queue",    "wait_hash",   "futex",        "ofd_table",
    "files",      "pipe",          "tty",          "net_stack",   "mm",           "vfs_namespace",
    "ofd_position", "fs_provider", "ram_topology", "page_pool",   "frame"};

[[nodiscard]] constexpr LockStat lock_stat(LockRank rank) noexcept
{
    switch (rank)
    {
    case LockRank::task_table:
        return LockStat::task_table;
    case LockRank::task:
        return LockStat::task;
    case LockRank::run_queue:
        return LockStat::run_queue;
    case LockRank::wait_hash:
        return LockStat::wait_hash;
    case LockRank::futex_bucket:
    case LockRank::futex_bucket_second:
        return LockStat::futex;
    case LockRank::ofd_table:
        return LockStat::ofd_table;
    case LockRank::files:
        return LockStat::files;
    case LockRank::pipe:
        return LockStat::pipe;
    case LockRank::tty:
        return LockStat::tty;
    case LockRank::net_stack:
        return LockStat::net_stack;
    case LockRank::mm:
        return LockStat::mm;
    case LockRank::vfs_namespace:
        return LockStat::vfs_namespace;
    case LockRank::ofd_position:
        return LockStat::ofd_position;
    case LockRank::fs_provider:
        return LockStat::fs_provider;
    case LockRank::ram_topology:
        return LockStat::ram_topology;
    case LockRank::page_pool:
        return LockStat::page_pool;
    case LockRank::frame:
        return LockStat::frame;
    default:
        return LockStat::none;
    }
}

// Per CPU and class: acquisitions (counted in debug builds only, where the
// lock-debug record sees every hold) and acquisitions that waited (both).
struct alignas(64) LockStatCpu
{
    std::array<std::atomic<uptr>, lock_stat_count> acquired{};
    std::array<std::atomic<uptr>, lock_stat_count> contended{};
};
inline constinit std::array<LockStatCpu, max_cpus> lock_stat_cpus{};

inline void count_lock_contended(LockRank rank, CpuId cpu) noexcept
{
    if (const auto stat = lock_stat(rank); stat != LockStat::none && cpu < max_cpus)
        detail::owner_add(lock_stat_cpus[cpu].contended[static_cast<usize>(stat)]);
}

[[nodiscard]] inline u64 lock_stat_total(LockStat stat, bool contended) noexcept
{
    u64 total = 0;
    if (stat == LockStat::none)
        return total;
    for (const auto &cpu : lock_stat_cpus)
        total += (contended ? cpu.contended : cpu.acquired)[static_cast<usize>(stat)].load(std::memory_order_relaxed);
    return total;
}

// Assertion counters of the lock order and its invariants
// (OK_SMP_KERNEL_CHECKS), in report order. The big kernel lock and wait-depth
// checks count in the section counters (ok/smp/kernel_section.hpp). A counter
// nothing feeds yet reports zero until the package that checks it lands.
enum class KernelCheck : u8
{
    held,               // check_held: the caller does not hold the lock it names.
    class_rule,         // I1/I2: a wait or class B acquisition under a class A lock.
    window_lock,        // A lock taken inside this CPU's user-access window.
    translation_writer, // A remap or destroy without the owning mm.
    top_half,           // A rank below top_half_floor taken in a top half.
    owner,              // An owner-field write from another CPU.
    queue,              // A thread queued twice, or queue state changed without its task lock.
    pin,                // Pin underflow, a pin across put_prev, a leaving CPU still pinned.
    admit_shadow_mismatch,
    quiesce_timeouts,
    audit_repairs,
    poll_audit_ready,
    count,
};
inline constexpr usize kernel_check_count = static_cast<usize>(KernelCheck::count);
inline constexpr std::array<std::string_view, kernel_check_count> kernel_check_names{
    "held_asserts",  "class_asserts", "window_lock_asserts",   "translation_writer_asserts",
    "top_half_asserts", "owner_asserts", "queue_asserts",      "pin_asserts",
    "admit_shadow_mismatch", "quiesce_timeouts", "audit_repairs", "poll_audit_ready"};

struct alignas(64) CheckCpu
{
    std::array<std::atomic<uptr>, kernel_check_count> counts{};
    // The first failed check's site on this CPU, for the report.
    std::atomic<const char *> first_site{};
};
inline constinit std::array<CheckCpu, max_cpus> check_cpus{};

inline void count_check(KernelCheck check, const char *site) noexcept
{
    const auto cpu = current_cpu();
    if (cpu >= max_cpus || check == KernelCheck::count)
        return;
    auto &self = check_cpus[cpu];
    detail::owner_add(self.counts[static_cast<usize>(check)]);
    if (self.first_site.load(std::memory_order_relaxed) == nullptr)
        self.first_site.store(site, std::memory_order_relaxed);
}

[[nodiscard]] inline u64 check_total(KernelCheck check) noexcept
{
    u64 total = 0;
    if (check == KernelCheck::count)
        return total;
    for (const auto &cpu : check_cpus)
        total += cpu.counts[static_cast<usize>(check)].load(std::memory_order_relaxed);
    return total;
}

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
    // The deepest nesting this CPU reached.
    u8 max_depth{0};
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
    if (record.depth > record.max_depth)
        record.max_depth = record.depth;
    if (const auto stat = lock_stat(rank); stat != LockStat::none)
        owner_add(lock_stat_cpus[cpu].acquired[static_cast<usize>(stat)]);
    if (rank < top_half_floor && (cpu_mode(cpu) & cpu_mode_top_half) != 0)
        count_check(KernelCheck::top_half, "lock below irq_router in a top half");
    if ((cpu_mode(cpu) & (cpu_mode_window | cpu_mode_top_half)) == cpu_mode_window)
        count_check(KernelCheck::window_lock, "lock taken inside a kernel window");
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
        lock_rank_violation({LockViolationKind::unlock_order, top != 0 ? record.ranks[top - 1] : rank,
                             index != 0 ? record.ranks[index - 1] : rank, lock, cpu});
    if (index == 0)
        return;
    for (usize next = index; next < top; ++next)
    {
        record.ranks[next - 1] = record.ranks[next];
        record.locks[next - 1] = record.locks[next];
    }
    --record.depth;
}

// True when this CPU holds a class A (spin) lock.
[[nodiscard]] inline bool lock_debug_holds_spin(CpuId cpu) noexcept
{
    if (cpu >= max_cpus)
        return false;
    const auto &record = lock_debug_cpus[cpu];
    for (usize index = 0; index < lock_debug_recorded(record); ++index)
        if (lock_class(record.ranks[index]) == LockClass::spin)
            return true;
    return false;
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

// The deepest nesting `cpu` reached (0 without OK_LOCK_DEBUG).
[[nodiscard]] inline usize lock_debug_max_depth([[maybe_unused]] CpuId cpu) noexcept
{
#if defined(OK_LOCK_DEBUG)
    return cpu < max_cpus ? lock_debug_cpus[cpu].max_depth : 0;
#else
    return 0;
#endif
}

// True when `cpu` records a hold above `rank` (W2: quiesce_root runs holding
// nothing above mm). Always false without OK_LOCK_DEBUG.
[[nodiscard]] inline bool lock_debug_holds_above([[maybe_unused]] CpuId cpu, [[maybe_unused]] LockRank rank) noexcept
{
#if defined(OK_LOCK_DEBUG)
    if (cpu >= max_cpus)
        return false;
    const auto &record = lock_debug_cpus[cpu];
    for (usize index = 0; index < detail::lock_debug_recorded(record); ++index)
        if (record.ranks[index] > rank)
            return true;
#endif
    return false;
}

// I1 and I2: no class B acquisition, device wait, user copy, quiesce or wait
// for another CPU while this CPU holds a class A lock. Counted as
// class_asserts where OK_LOCK_DEBUG records holds; false on a violation.
inline bool check_class_wait([[maybe_unused]] const char *site) noexcept
{
#if defined(OK_LOCK_DEBUG)
    if (detail::lock_debug_holds_spin(current_cpu()))
    {
        count_check(KernelCheck::class_rule, site);
        return false;
    }
#endif
    return true;
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
        return acquire(Rank);
    }

    // Records this one acquisition at `As` instead of the lock's own rank
    // (Linux spin_lock_nested): the second of two same-rank locks a defined
    // order takes, such as the higher futex bucket of a pair.
    template <LockRank As> bool lock_as() noexcept
    {
        static_assert(As > Rank, "lock_as records a rank above the lock's own");
        static_assert(lock_class(As) == lock_class(Rank), "lock_as keeps the lock's class");
        return acquire(As);
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

    bool acquire([[maybe_unused]] LockRank recorded) noexcept
    {
        const auto cpu = current_cpu();
#if defined(OK_LOCK_DEBUG)
        if (!detail::lock_debug_may_wait(this, recorded, cpu,
                                         owner_.load(std::memory_order_relaxed) == owner_value(cpu)))
            return false;
#endif
        bool waited = false;
        if (locked_.test_and_set(std::memory_order_acquire))
        {
            waited = true;
            count_lock_contended(recorded, cpu);
            const SpinningScope spinning;
            do
            {
                while (locked_.test(std::memory_order_relaxed))
                    arch::ArchOperations::spin_pause();
            } while (locked_.test_and_set(std::memory_order_acquire));
        }
        owner_.store(owner_value(cpu), std::memory_order_relaxed);
#if defined(OK_LOCK_DEBUG)
        detail::lock_debug_acquired(this, recorded, cpu);
#endif
        return waited;
    }

    std::atomic_flag locked_{};
    std::atomic<u32> owner_{0};
};

// The same interface on a profile that runs one CPU: nothing can contend, so
// it never spins. Debug builds keep the owner word and the order checks;
// release builds keep one plain held byte, so try_lock and
// owned_by_current_cpu answer as a RankedSpinLock's do in both modes.
template <LockRank Rank> class UpLock final
{
  public:
    static constexpr LockRank rank = Rank;

    constexpr UpLock() noexcept = default;
    UpLock(const UpLock &) = delete;
    UpLock &operator=(const UpLock &) = delete;

    // Never waits.
    bool lock() noexcept
    {
        acquire(Rank);
        return false;
    }

    template <LockRank As> bool lock_as() noexcept
    {
        static_assert(As > Rank, "lock_as records a rank above the lock's own");
        static_assert(lock_class(As) == lock_class(Rank), "lock_as keeps the lock's class");
        acquire(As);
        return false;
    }

    void unlock() noexcept
    {
#if defined(OK_LOCK_DEBUG)
        const auto cpu = current_cpu();
        if (owner_ != owner_value(cpu))
        {
            lock_rank_violation({LockViolationKind::not_owner, Rank, Rank, this, cpu});
            return;
        }
        if (detail::lock_debug_consume_refused(this, cpu))
            return;
        detail::lock_debug_released(this, Rank, cpu);
        owner_ = 0;
#else
        held_ = false;
#endif
    }

    // Fails while the lock is held, as a RankedSpinLock's try does.
    [[nodiscard]] bool try_lock() noexcept
    {
#if defined(OK_LOCK_DEBUG)
        if (owner_ != 0)
            return false;
        const auto cpu = current_cpu();
        owner_ = owner_value(cpu);
        detail::lock_debug_check_order(this, Rank, cpu);
        detail::lock_debug_acquired(this, Rank, cpu);
#else
        if (held_)
            return false;
        held_ = true;
#endif
        return true;
    }

    // One CPU runs, so a held lock is this CPU's.
    [[nodiscard]] bool owned_by_current_cpu() const noexcept
    {
#if defined(OK_LOCK_DEBUG)
        return owner_ == owner_value(current_cpu());
#else
        return held_;
#endif
    }

    class Guard final
    {
      public:
        explicit Guard(UpLock &lock) noexcept : lock_(lock)
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
        UpLock &lock_;
    };

  private:
    [[nodiscard]] static constexpr u32 owner_value(CpuId cpu) noexcept
    {
        return static_cast<u32>(cpu) + 1;
    }

    void acquire([[maybe_unused]] LockRank recorded) noexcept
    {
#if defined(OK_LOCK_DEBUG)
        const auto cpu = current_cpu();
        if (!detail::lock_debug_may_wait(this, recorded, cpu, owner_ == owner_value(cpu)))
            return;
        owner_ = owner_value(cpu);
        detail::lock_debug_acquired(this, recorded, cpu);
#else
        held_ = true;
#endif
    }

#if defined(OK_LOCK_DEBUG)
    u32 owner_{0};
#else
    bool held_{false};
#endif
};

// The lock of a class A structure: a spin lock where CPUs run concurrently
// (or a hosted race test models them), an UpLock elsewhere. Selected at build
// time; both satisfy the run queue's RunQueueLock shape.
template <LockRank Rank> using FineLock = std::conditional_t<concurrent_locks, RankedSpinLock<Rank>, UpLock<Rank>>;

template <typename L>
concept RankedLock = requires(L &lock, const L &view) {
    { L::rank } -> std::convertible_to<LockRank>;
    lock.lock();
    lock.unlock();
    { lock.try_lock() } -> std::same_as<bool>;
    { view.owned_by_current_cpu() } -> std::same_as<bool>;
};
static_assert(RankedLock<RankedSpinLock<LockRank::task>>);
static_assert(RankedLock<UpLock<LockRank::task>>);

// A held-lock annotation: counts held_asserts where OK_LOCK_DEBUG is defined
// and does nothing otherwise.
template <typename L>
    requires requires(const L &lock) {
        { lock.owned_by_current_cpu() } -> std::same_as<bool>;
    }
inline bool check_held([[maybe_unused]] const L &lock, [[maybe_unused]] const char *site) noexcept
{
    if constexpr (lock_debug_enabled)
        if (!lock.owned_by_current_cpu())
        {
            count_check(KernelCheck::held, site);
            return false;
        }
    return true;
}

// An unranked try-only token (hotplug_turn, housekeeping): an atomic flag and
// an owner word. Nobody waits for it, so it stays out of the lock-debug
// record and its holder may release and retake the big kernel lock while it
// holds the token. check_held works through the owner word.
class TryToken final
{
  public:
    constexpr TryToken() noexcept = default;
    TryToken(const TryToken &) = delete;
    TryToken &operator=(const TryToken &) = delete;

    [[nodiscard]] bool try_lock() noexcept
    {
        if (taken_.test(std::memory_order_relaxed) || taken_.test_and_set(std::memory_order_acquire))
            return false;
        owner_.store(static_cast<u32>(current_cpu()) + 1, std::memory_order_relaxed);
        return true;
    }

    void unlock() noexcept
    {
        owner_.store(0, std::memory_order_relaxed);
        taken_.clear(std::memory_order_release);
    }

    [[nodiscard]] bool owned_by_current_cpu() const noexcept
    {
        return owner_.load(std::memory_order_relaxed) == static_cast<u32>(current_cpu()) + 1;
    }

    [[nodiscard]] bool held() const noexcept
    {
        return taken_.test(std::memory_order_relaxed);
    }

  private:
    std::atomic_flag taken_{};
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
