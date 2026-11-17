#pragma once

// The user-facing CPU hotplug service (docs/modules/sysfs.md). A write of
// "0" or "1" to /sys/devices/system/cpu/cpuN/online files a request in the
// writer's record and waits; CPU 0's kernel loop serves one request per pass
// with stop_secondary_cpu/restart_secondary_cpu and completes the writer's
// wait with the result, which the writer consumes once when it re-dispatches.
//
// HotplugService is the record table and the pass, over a Backend that
// supplies the CPU facts and actions: the kernel's in src/smp/hotplug.cpp,
// hosted models' in tests/host/cpu_hotplug_service_tests.cpp.

#include "ok/core/types.hpp"
#include "ok/sched/wait_registration.hpp"
#include "ok/smp/lock.hpp"
#include "ok/smp/smp.hpp"

#include <array>
#include <atomic>
#include <concepts>
#include <optional>
#include <span>

namespace ok { class Kernel; }

namespace ok::smp
{
// What /sys/devices/system/cpu shows: the CPUs bring-up started (a CPU whose
// start failed is absent) and those online now.
struct CpuView
{
    u32 present{1};
    u32 online{1};
    constexpr bool operator==(const CpuView &) const = default;
};

// A CPU is online for userland when its state is online, it is in the online
// mask and the scheduler places threads on it. A stop that timed out leaves
// the CPU stopping with the scheduler bit cleared, so it reads offline.
[[nodiscard]] bool cpu_online_now(CpuId cpu) noexcept;
[[nodiscard]] CpuView cpu_view() noexcept;
// Bring-up names the kernel whose scheduler and topology the service uses.
void hotplug_attach(Kernel &kernel) noexcept;
// CPU 0's kernel loop, holding the kernel lock: serves at most one request.
// Returns whether it took one; false on profiles that start no secondaries.
bool hotplug_service_pass() noexcept;

namespace hotplug
{
// One record per entry-stack lease slot: a native thread owns its lease for
// its lifetime, so it owns at most one request. The pass serves one request
// at a time; when its writer leaves meanwhile, the record moves to one spare
// past the slots, so the lease slot is free for its next owner at once.
inline constexpr usize record_capacity = 128;

inline constexpr i64 errno_busy = 16, errno_io = 5, errno_invalid = 22, errno_too_big = 7;
// Linux accepts a write of at most a page to a sysfs attribute.
inline constexpr usize max_write = 4096;
// kstrtobool reads at most the first two bytes; a few more keep "on\n".
inline constexpr usize parsed_bytes = 16;

enum class RecordState : u8
{
    free,
    armed,     // Filed; the writer waits.
    running,   // Taken by the service pass.
    done,      // Result stored; the writer consumes it once.
    abandoned, // The writer left while the service ran; the service frees it.
};

struct Record
{
    RecordState state{RecordState::free};
    u32 sequence{};   // Order of filing: the pass serves the oldest first.
    u32 generation{}; // Changes whenever the slot is filed again.
    CpuId cpu{};
    bool want{};
    u64 pid{}, tid{};
    sched::WaitTicket ticket{};
    i64 count{};  // The write's byte count, the success result.
    i64 result{}; // Valid when done: count or a negative errno.
};

struct Request
{
    CpuId cpu{};
    bool want{};
    u64 pid{}, tid{};
    sched::WaitTicket ticket{};
    i64 count{};
};

// A request either completes at once (a no-op, or refused) or is armed.
struct Submission
{
    bool armed{};
    i64 result{}; // When not armed: count or a negative errno.
};

// Reported as OK_SMP_HOTPLUG_USER; CPU 0 alone writes the service counters,
// the writers' only under the record lock.
struct Statistics
{
    u64 requests{}, offline{}, online{}, noop{}, failures{}, busy{}, orphaned{}, wedged{}, max_us{};
    // Reported as OK_SMP_HOTPLUG_RECORDS: requests whose writer left while
    // the service ran them (freed by the service, nothing completed), and
    // armed or finished requests freed at the writer's teardown.
    u64 abandoned{}, dropped{};
};

// Why a request is refused before anything is attempted.
enum class Refusal : u8
{
    none,
    busy,     // The debug knob cycles this CPU, shutdown is pending, or the CPU is in a transition.
    io_error, // The CPU is halted.
};

template <typename B>
concept Backend = requires(B &backend, const B &view, CpuId cpu, u64 id, sched::WaitTicket ticket) {
    { view.online(cpu) } -> std::same_as<bool>;
    { view.refusal(cpu) } -> std::same_as<Refusal>;
    // The writer still waits on `ticket` for this request.
    { backend.owner_waiting(id, id, ticket) } -> std::same_as<bool>;
    { backend.stop(cpu) } -> std::same_as<Status>;
    { backend.restart(cpu) } -> std::same_as<Status>;
    // Completes the writer's wait (ready); holds no record lock.
    { backend.complete(ticket) } -> std::same_as<void>;
    // A CPU whose stop timed out: true once it parked after all, which the
    // backend records (topology offline) so that a later "1" restarts it.
    { backend.recover(cpu) } -> std::same_as<bool>;
    { view.now_ns() } -> std::same_as<u64>;
};

// kstrtobool: 1yYtT and "on" are true, 0nNfF and "of..." are false; only the
// first two bytes count. Anything else is EINVAL (nullopt).
[[nodiscard]] constexpr std::optional<bool> parse_bool(std::span<const char> text) noexcept
{
    if (text.empty()) return std::nullopt;
    switch (text[0])
    {
    case '1': case 'y': case 'Y': case 't': case 'T': return true;
    case '0': case 'n': case 'N': case 'f': case 'F': return false;
    case 'o': case 'O':
        if (text.size() < 2) return std::nullopt;
        if (text[1] == 'n' || text[1] == 'N') return true;
        if (text[1] == 'f' || text[1] == 'F') return false;
        return std::nullopt;
    default: return std::nullopt;
    }
}
static_assert(*parse_bool(std::span<const char>{"1\n", 2}) && !*parse_bool(std::span<const char>{"0", 1}));
static_assert(*parse_bool(std::span<const char>{"on", 2}) && !*parse_bool(std::span<const char>{"off\n", 4}));
static_assert(!parse_bool(std::span<const char>{"x", 1}) && !parse_bool(std::span<const char>{"o", 1}));

template <Backend B> class HotplugService final
{
  public:
    constexpr HotplugService() = default;
    HotplugService(const HotplugService &) = delete;
    HotplugService &operator=(const HotplugService &) = delete;

    // The writer, after registering its wait (so a result never races the
    // registration), holding no lock but the record lock taken here.
    Submission submit(const B &backend, usize slot, const Request &request)
    {
        if (slot >= record_capacity) return {false, -errno_invalid};
        const typename Lock::Guard guard(lock_);
        ++filed_.requests;
        // Linux returns success for a write that changes nothing.
        if (backend.online(request.cpu) == request.want)
        {
            ++filed_.noop;
            return {false, request.count};
        }
        switch (backend.refusal(request.cpu))
        {
        case Refusal::busy: ++filed_.busy; return {false, -errno_busy};
        case Refusal::io_error: ++filed_.failures; return {false, -errno_io};
        case Refusal::none: break;
        }
        auto &record = records_[slot];
        // Only its own writer, which waits for it, can find a running record
        // in its slot (a writer that left moved it to the spare): refuse.
        if (record.state == RecordState::running || record.state == RecordState::abandoned)
        {
            ++filed_.busy;
            return {false, -errno_busy};
        }
        // A result its owner never consumed (the descriptor went away before
        // the re-dispatch) is dropped with the new request.
        free_locked(slot);
        record = {.state = RecordState::armed, .sequence = ++sequence_, .generation = record.generation + 1,
                  .cpu = request.cpu, .want = request.want, .pid = request.pid, .tid = request.tid,
                  .ticket = request.ticket, .count = request.count, .result = 0};
        pending_.store(pending_.load(std::memory_order_relaxed) + 1, std::memory_order_release);
        return {true, 0};
    }

    // The writer's re-dispatch: the stored result of its current ticket, once.
    std::optional<i64> consume(usize slot, sched::WaitTicket ticket)
    {
        if (slot >= record_capacity) return std::nullopt;
        const typename Lock::Guard guard(lock_);
        auto &record = records_[slot];
        if (record.state != RecordState::done || record.ticket != ticket) return std::nullopt;
        const auto result = record.result;
        free_locked(slot);
        return result;
    }

    // Lock-free hint for the step: the slot holds a result not yet consumed.
    [[nodiscard]] bool finished(usize slot) const noexcept
    {
        return slot < record_capacity && finished_[slot].load(std::memory_order_acquire);
    }

    // Teardown of the writer's thread, or exec replacing its image: an armed
    // or done record is freed; a running one moves to the spare, abandoned,
    // and the service frees it. The lease slot is free either way.
    void abandon(usize slot, u64 pid, u64 tid)
    {
        if (slot >= record_capacity) return;
        const typename Lock::Guard guard(lock_);
        auto &record = records_[slot];
        if (record.pid != pid || record.tid != tid) return;
        if (record.state == RecordState::armed || record.state == RecordState::done)
        {
            ++filed_.dropped;
            free_locked(slot);
        }
        else if (record.state == RecordState::running && serving_ == slot)
        {
            records_[spare] = record;
            records_[spare].state = RecordState::abandoned;
            serving_ = spare;
            free_locked(slot);
        }
    }

    // One pass on CPU 0: recover wedged CPUs, then serve the oldest armed
    // request. Returns whether a request was taken.
    bool pass(B &backend)
    {
        for (CpuId cpu = 1; cpu < max_cpus; ++cpu)
            if ((wedged_ & (u32{1} << cpu)) != 0 && backend.recover(cpu)) wedged_ &= ~(u32{1} << cpu);
        // CS1: take the oldest armed record.
        Record taken{};
        usize slot = record_capacity;
        {
            const typename Lock::Guard guard(lock_);
            for (usize index = 0; index < record_capacity; ++index)
                if (records_[index].state == RecordState::armed &&
                    (slot == record_capacity || records_[index].sequence - records_[slot].sequence > 0x8000'0000u))
                    slot = index;
            if (slot == record_capacity) return false;
            records_[slot].state = RecordState::running;
            pending_.store(pending_.load(std::memory_order_relaxed) - 1, std::memory_order_release);
            taken = records_[slot];
            serving_ = slot;
        }
        // CS2: the writer must still wait for this request.
        if (!backend.owner_waiting(taken.pid, taken.tid, taken.ticket))
        {
            const typename Lock::Guard guard(lock_);
            auto &record = records_[serving_];
            if (record.generation == taken.generation)
            {
                ++(record.state == RecordState::abandoned ? served_.abandoned : served_.orphaned);
                free_locked(serving_);
            }
            serving_ = idle;
            return true;
        }
        const auto result = serve(backend, taken);
        // CS3: store the result unless the writer left meanwhile (the record
        // is then the abandoned spare).
        bool deliver = false;
        {
            const typename Lock::Guard guard(lock_);
            const usize index = serving_;
            auto &record = records_[index];
            if (record.generation == taken.generation && record.state == RecordState::running)
            {
                record.result = result;
                record.state = RecordState::done;
                finished_[index].store(true, std::memory_order_release);
                deliver = true;
            }
            else if (record.generation == taken.generation && record.state == RecordState::abandoned)
            {
                ++served_.abandoned;
                free_locked(index);
            }
            serving_ = idle;
        }
        if (deliver) backend.complete(taken.ticket);
        return true;
    }

    [[nodiscard]] u32 pending() const noexcept { return pending_.load(std::memory_order_acquire); }
    [[nodiscard]] u32 wedged() const noexcept { return wedged_; }
    // CPU 0 reads it; the writers' share is taken under the record lock.
    [[nodiscard]] Statistics statistics()
    {
        Statistics total = served_;
        const typename Lock::Guard guard(lock_);
        total.requests += filed_.requests;
        total.noop += filed_.noop;
        total.busy += filed_.busy;
        total.failures += filed_.failures;
        total.dropped += filed_.dropped;
        return total;
    }
    // A slot's record; `spare` names the abandoned record the pass still runs.
    [[nodiscard]] Record record(usize slot)
    {
        if (slot > spare) return {};
        const typename Lock::Guard guard(lock_);
        return records_[slot];
    }

    static constexpr usize spare = record_capacity;

  private:
    using Lock = FineLock<LockRank::hotplug_request>;
    static constexpr usize idle = record_capacity + 1;

    void free_locked(usize slot)
    {
        auto &record = records_[slot];
        if (record.state == RecordState::armed)
            pending_.store(pending_.load(std::memory_order_relaxed) - 1, std::memory_order_release);
        record.state = RecordState::free;
        record.ticket = {};
        record.pid = record.tid = 0;
        finished_[slot].store(false, std::memory_order_release);
    }

    // Holding no record lock: the state is checked again, then the action
    // runs and its outcome becomes the result. The CPU state is mapped first
    // (no-op, refusal), then the Status.
    i64 serve(B &backend, const Record &taken)
    {
        if (backend.online(taken.cpu) == taken.want)
        {
            ++served_.noop;
            return taken.count;
        }
        switch (backend.refusal(taken.cpu))
        {
        case Refusal::busy: ++served_.busy; return -errno_busy;
        case Refusal::io_error: ++served_.failures; return -errno_io;
        case Refusal::none: break;
        }
        const auto started = backend.now_ns();
        const auto status = taken.want ? backend.restart(taken.cpu) : backend.stop(taken.cpu);
        const auto elapsed = (backend.now_ns() - started) / 1000;
        if (elapsed > served_.max_us) served_.max_us = elapsed;
        if (status)
        {
            ++(taken.want ? served_.online : served_.offline);
            return taken.count;
        }
        switch (status.code())
        {
        case StatusCode::busy: // The hotplug turn, or a process mask keeps the CPU.
            ++served_.busy;
            return -errno_busy;
        case StatusCode::io_error:
            // A stop that timed out leaves the CPU stopping: wedged until it parks.
            if (!taken.want && !backend.online(taken.cpu) && backend.refusal(taken.cpu) == Refusal::busy)
            {
                wedged_ |= u32{1} << taken.cpu;
                ++served_.wedged;
            }
            ++served_.failures;
            return -errno_io;
        default: // fault (it left with a record), or an inconsistent state.
            ++served_.failures;
            return -errno_io;
        }
    }

    Lock lock_{};
    // The lease slots, then the spare.
    std::array<Record, record_capacity + 1> records_{};
    std::array<std::atomic<bool>, record_capacity + 1> finished_{};
    usize serving_{idle}; // The record the pass runs, under the record lock.
    u32 sequence_{};
    std::atomic<u32> pending_{};
    u32 wedged_{};
    Statistics filed_{};  // Writers, under the record lock.
    Statistics served_{}; // CPU 0's pass.
};

// The kernel's service (src/smp/hotplug.cpp).
Submission submit(usize slot, const Request &request);
std::optional<i64> consume(usize slot, sched::WaitTicket ticket);
[[nodiscard]] bool finished(usize slot) noexcept;
void abandon(usize slot, u64 pid, u64 tid);
// Requests filed and not yet taken; CPU 0 does not idle while one waits.
[[nodiscard]] u32 pending() noexcept;
[[nodiscard]] Statistics statistics() noexcept;
} // namespace hotplug
} // namespace ok::smp
