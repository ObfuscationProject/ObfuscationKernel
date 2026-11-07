#include "ok/smp/bringup.hpp"

#include "ok/arch/arch.hpp"
#include "ok/arch/clock.hpp"
#include "ok/arch/execution.hpp"
#include "ok/arch/smp.hpp"
#include "ok/interrupt/platform.hpp"
#include "ok/core/kernel.hpp"
#include "ok/memory/memory.hpp"
#include "ok/core/native_shutdown.hpp"
#include "ok/smp/cpu.hpp"
#include "ok/smp/kernel_section.hpp"
#include "../arch/tlb_tags.hpp"

#include <array>

extern "C" void ok_platform_console_write_char(char value) __attribute__((weak));

namespace ok::smp
{
namespace
{

// Each secondary runs its kernel loop, syscalls included, on its own stack.
constexpr usize secondary_stack_pages = 64;
constexpr u64 start_timeout_ns = 2'000'000'000;
constexpr u64 ping_timeout_ns = 500'000'000;

constinit BringupReport report{};
constinit memory::ChargeAccount stack_charges{};
bool attempted{};
// The kernel bring-up served, for the hotplug test's passes.
[[maybe_unused]] Kernel *bringup_kernel{};

void console(std::string_view text)
{
    if (ok_platform_console_write_char == nullptr)
        return;
    for (char value : text)
        ok_platform_console_write_char(value);
}

void console_number(u64 value)
{
    char digits[20];
    usize count = 0;
    do
    {
        digits[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);
    while (count != 0)
        console({&digits[--count], 1});
}

void emit_report()
{
    console("OK_SMP scope=hardware cpus=");
    console_number(report.discovered);
    console(" online=");
    console_number(report.online);
    console(" ap_start=");
    console(report.method);
    console(" ipi=");
    console(report.ipi);
    if (!report.reason.empty())
    {
        console(" reason=");
        console(report.reason);
    }
    if (const auto tsc = arch::smp::clock_sync_name(); !tsc.empty())
    {
        console(" tsc=");
        console(tsc);
    }
    console("\n");
}

// How user entries treat the TLB on the started CPUs: the narrowest ASID width
// (none: every entry flushes the user TLB) and whether x86 PCIDs carry it.
void emit_tlb_report(u32 online)
{
    const auto tags = arch::execution::tlb_tag_report(online);
    if (!tags.supported)
        return;
    console("OK_SMP_TLB scope=hardware asid=");
    if (tags.asid_bits == 0)
        console("none");
    else
        console_number(tags.asid_bits);
    console(tags.pcid ? " pcid=on\n" : " pcid=off\n");
}

// Spins until `done` holds or `timeout_ns` passed on the boot CPU's clock.
template <typename Done> bool wait_for(Done done, u64 timeout_ns)
{
    const auto start = arch::monotonic_nanoseconds();
    if (!start)
        return done();
    for (;;)
    {
        if (done())
            return true;
        const auto now = arch::monotonic_nanoseconds();
        if (!now || now.value() - start.value() >= timeout_ns)
            return done();
        arch::ArchOperations::spin_pause();
    }
}

void set_state(CpuId cpu, CpuState state)
{
    cpu_record(cpu).state.store(static_cast<u8>(state), std::memory_order_release);
}

[[nodiscard]] CpuState state_of(CpuId cpu)
{
    return static_cast<CpuState>(cpu_record(cpu).state.load(std::memory_order_acquire));
}

#if defined(OK_ARCH_SMP_START)
constexpr u64 stop_timeout_ns = 2'000'000'000;

// Stops and restarts run one at a time: x86 starts share one trampoline page.
constinit std::atomic_flag hotplug_busy = ATOMIC_FLAG_INIT;
class HotplugTurn
{
  public:
    HotplugTurn() noexcept : owned_(!hotplug_busy.test_and_set(std::memory_order_acquire)) {}
    ~HotplugTurn()
    {
        if (owned_)
            hotplug_busy.clear(std::memory_order_release);
    }
    HotplugTurn(const HotplugTurn &) = delete;
    HotplugTurn &operator=(const HotplugTurn &) = delete;
    explicit operator bool() const noexcept { return owned_; }

  private:
    bool owned_;
};

// Set by a leaving CPU that still found a user entry or a pinned section;
// read by its requester once it parked.
constinit std::array<std::atomic<bool>, max_cpus> leave_faults{};

// The caller holds the kernel lock: releases it while `done` does not hold,
// for at most `timeout_ns`, serving this CPU's interrupts meanwhile (lines
// routed here, IPIs), and takes it again.
template <typename Done> bool wait_unlocked(Done done, u64 timeout_ns)
{
    bkl_unlock();
    const bool met = wait_for(
        [&] {
            interrupt::serve_local_interrupts();
            return done();
        },
        timeout_ns);
    interrupt::lock_kernel_serving_interrupts();
    return met;
}

[[nodiscard]] Status hotplug_request(CpuId cpu)
{
    if (cpu == 0 || cpu >= max_cpus || cpu == current_cpu())
        return Status::invalid_argument("hotplug acts on a secondary CPU other than the caller");
    if (!kernel_lock.owned_by_current_cpu())
        return Status::denied("hotplug runs under the kernel lock");
    if ((started_mask() & (u32{1} << cpu)) == 0)
        return Status::invalid_argument("hotplug acts on a CPU bring-up started");
    return Status::success();
}

// The CPU leaving, at a loop pass outside user code (stop_secondary_cpu).
[[noreturn]] void leave(CpuId cpu)
{
    auto &record = cpu_record(cpu);
    interrupt::lock_kernel_serving_interrupts();
    bool clean = ok_smp_cpu_leaving == nullptr || ok_smp_cpu_leaving(cpu);
    // Its last section ended with its last step; it returns with an idle record.
    clean = section_records[cpu].load() == SectionState::idle && clean;
    section_records[cpu].store(SectionState::idle);
    // Out of the online mask first: IPIs and shootdowns stop covering it, and
    // every routable chip refuses it as a target (route() checks the mask
    // under the router lock), so neither bind_platform_irq nor a top half's
    // follow() can place a line here once the lines move away below. A
    // follow() that routed one here before still ran under that lock, ahead
    // of the move.
    detail::online.fetch_and(~(u32{1} << cpu), std::memory_order_seq_cst);
    static_cast<void>(interrupt::retarget_platform_irqs_away(cpu));
    bkl_unlock();
    // What reached it before the others saw it leave, then nothing more.
    interrupt::serve_local_interrupts();
    arch::smp::stop_local();
    record.ipi_pending.store(0, std::memory_order_release);
    leave_faults[cpu].store(!clean, std::memory_order_relaxed);
    set_state(cpu, CpuState::offline);
    arch::smp::park_self(cpu);
}
#endif

#if defined(OK_SMP_HOTPLUG_TEST) && defined(OK_ARCH_SMP_START)
// Online time of the cycled CPU between two cycles.
constexpr u64 hotplug_period_ns = 100'000'000;
constinit HotplugStatistics hotplug_stats{};
u64 hotplug_next{};
#endif

#if defined(OK_ENABLE_TEST_POINTS) && defined(OK_ARCH_SMP_START)
// How long a parked CPU must stay quiet after an IPI raised at it.
constexpr u64 parked_quiet_ns = 20'000'000;

// Debug test point before userland: CPU 1 leaves, takes no IPI while parked,
// returns, and answers an IPI again.
void boot_hotplug_check(Kernel &kernel)
{
    constexpr CpuId cpu = 1;
    constexpr u32 bit = u32{1} << cpu;
    if ((online_mask() & bit) == 0)
        return;
    const bool stopped = stop_secondary_cpu(kernel, cpu).ok() && (online_mask() & bit) == 0 &&
                         (kernel.scheduler().online_cpus() & bit) == 0 && state_of(cpu) == CpuState::offline;
    // Parked: send_ipi() skips it, and an IPI raised at its hardware anyway
    // is not taken (x86 halts with interrupts off, aarch64 is off, the rv64
    // park loop only rereads its mailbox). The restart discards what stayed
    // pending (INIT, GicV2::reset_cpu_lines, the park loop's MSIP clear).
    bool quiet = false;
    if (stopped)
    {
        auto &target = cpu_record(cpu);
        const auto sent = cpu_record(current_cpu()).ipis_sent.load(std::memory_order_relaxed);
        const auto received = target.ipis_received.load(std::memory_order_acquire);
        send_ipi(cpu, ipi_ping);
        const bool skipped = cpu_record(current_cpu()).ipis_sent.load(std::memory_order_relaxed) == sent &&
                             target.ipi_pending.load(std::memory_order_acquire) == 0;
        if (detail::ipi_sender != nullptr)
            detail::ipi_sender(cpu, target.hardware_id);
        static_cast<void>(wait_for([] { return false; }, parked_quiet_ns));
        quiet = skipped && target.ipis_received.load(std::memory_order_acquire) == received &&
                state_of(cpu) == CpuState::offline && arch::smp::parked(cpu, target.hardware_id);
    }
    const bool restarted = stopped && restart_secondary_cpu(kernel, cpu).ok() && (online_mask() & bit) != 0 &&
                           (kernel.scheduler().online_cpus() & bit) != 0;
    bool delivered = false;
    if (restarted)
    {
        // Before userland the CPU idles in wait_local, which takes the IPI.
        const auto before = cpu_record(cpu).ipis_received.load(std::memory_order_acquire);
        send_ipi(cpu, ipi_ping);
        delivered = wait_for([&] { return cpu_record(cpu).ipis_received.load(std::memory_order_acquire) != before; },
                             ping_timeout_ns);
    }
    const auto verdict = [](bool pass) { return pass ? std::string_view{"pass"} : std::string_view{"fail"}; };
    console("OK_SMP_HOTPLUG_BOOT scope=hardware cpu=1 stop=");
    console(verdict(stopped));
    console(" parked_ipi=");
    console(verdict(quiet));
    console(" restart=");
    console(verdict(restarted));
    console(" ipi=");
    console(verdict(delivered));
    console("\n");
}
#endif

} // namespace

const BringupReport &bringup_report() noexcept
{
    return report;
}

Status start_secondary_cpus(Kernel &kernel)
{
    if (attempted)
        return Status::already_exists("secondary CPU bring-up already ran");
    attempted = true;
    set_state(0, CpuState::boot);
    cpu_record(0).hardware_id = arch::smp::current_hardware_id();
    cpu_record(0).reported_id.store(static_cast<uptr>(cpu_record(0).hardware_id), std::memory_order_release);

    boot::CpuTopologyMap map{};
    auto discovered_status = arch::smp::discover(map);
    report.method = arch::smp::start_method_name(map);
    if (!discovered_status || map.count <= 1 || map.method == boot::CpuStartMethod::none)
    {
        report.discovered = map.count == 0 ? 1 : map.count;
        report.reason = !discovered_status      ? std::string_view{"discovery-failed"}
                        : !map.up_reason.empty() ? map.up_reason
                                                 : std::string_view{"single-cpu"};
        report.method = "unsupported";
        emit_report();
        return Status::success();
    }
    report.discovered = map.count;
    detail::discovered.store(map.count, std::memory_order_release);

    // Secondaries use the execution backend's vectors, translation and per-CPU
    // tables from their first instruction, and the boot CPU's clock bounds the
    // waits below.
    if (auto status = arch::execution::initialize(); !status)
        return status;
    if (auto status = arch::initialize_clock(); !status)
        return status;
    if (auto status = arch::smp::prepare_boot_cpu(map); !status)
        return status;
    detail::ipi_sender = &arch::smp::send_ipi;
    detail::ipi_wake = []() noexcept { ok_interrupt_wake(); };

    // Logical ids follow the firmware order with the boot CPU as 0.
    CpuId next = 1;
    u32 started_mask = 1;
    for (usize index = 0; index < map.count && next < max_cpus; ++index)
    {
        const auto &cpu = map.cpus[index];
        if (index == map.boot_index || !cpu.enabled)
            continue;
        const CpuId logical = next++;
        auto &record = cpu_record(logical);
        record.hardware_id = cpu.hardware_id;
        auto stack = kernel.memory().frames().allocate_contiguous(secondary_stack_pages, stack_charges);
        if (!stack)
        {
            set_state(logical, CpuState::offline);
            continue;
        }
        record.stack_lower = stack.value().address;
        record.stack_top = stack.value().address + secondary_stack_pages * arch::execution::page_size;
        set_state(logical, CpuState::starting);
        if (auto status = arch::smp::start(logical, cpu.hardware_id, record.stack_lower, record.stack_top); !status)
        {
            set_state(logical, CpuState::offline);
            continue;
        }
        // Online only once the CPU itself ran C++ and reported the identity it
        // reads from its own registers.
        const bool came_online = wait_for([&] { return state_of(logical) == CpuState::online; }, start_timeout_ns);
        if (came_online && record.reported_id.load(std::memory_order_acquire) == cpu.hardware_id)
            started_mask |= u32{1} << logical;
        else
            set_state(logical, came_online ? CpuState::halted : CpuState::offline);
    }
    detail::started.store(started_mask, std::memory_order_release);
    detail::online.store(started_mask, std::memory_order_release);
    report.online = online_cpus();

    // Delivery check: every online secondary acknowledges one ping.
    bool delivered = report.online > 1;
    for (CpuId cpu = 1; cpu < max_cpus; ++cpu)
    {
        if ((started_mask & (u32{1} << cpu)) == 0)
            continue;
        const auto before = cpu_record(cpu).ipis_received.load(std::memory_order_acquire);
        send_ipi(cpu, ipi_ping);
        delivered = wait_for([&] { return cpu_record(cpu).ipis_received.load(std::memory_order_acquire) != before; },
                             ping_timeout_ns) &&
                    delivered;
    }
    report.ipi = delivered ? "pass" : "fail";
    if (auto status = kernel.publish_cpu_topology(map.count, started_mask); !status)
        return status;
    if (report.online != map.count)
        report.reason = "secondary-start-failed";
    emit_report();
    emit_tlb_report(started_mask);
    bringup_kernel = &kernel;
#if defined(OK_ENABLE_TEST_POINTS) && defined(OK_ARCH_SMP_START)
    boot_hotplug_check(kernel);
#endif
    return Status::success();
}

Status stop_secondary_cpu([[maybe_unused]] Kernel &kernel, [[maybe_unused]] CpuId cpu)
{
#if defined(OK_ARCH_SMP_START)
    if (auto status = hotplug_request(cpu); !status)
        return status;
    const HotplugTurn turn;
    if (!turn)
        return Status::busy("another CPU hotplug request is running");
    const u32 bit = u32{1} << cpu;
    if (state_of(cpu) != CpuState::online || (online_mask() & bit) == 0)
        return Status::invalid_argument("CPU is not online");
    auto &record = cpu_record(cpu);
    leave_faults[cpu].store(false, std::memory_order_relaxed);
    set_state(cpu, CpuState::stopping);
    // No thread is placed on it any more; its queued ones move now.
    if (auto status = kernel.scheduler().offline_cpu(cpu); !status)
    {
        set_state(cpu, CpuState::online);
        return status;
    }
    drain_cpu(cpu);
    // Out of user mode: it then needs the kernel lock to finish that entry,
    // so only its window is waited for here, the rest with the lock released.
    const auto generation = record.user_generation.load(std::memory_order_acquire);
    send_ipi(cpu, ipi_kick | ipi_reschedule);
    if ((generation & 1) != 0)
        static_cast<void>(wait_for(
            [&] { return record.user_generation.load(std::memory_order_acquire) != generation; }, stop_timeout_ns));
    if (!wait_unlocked(
            [&] { return state_of(cpu) == CpuState::offline && arch::smp::parked(cpu, record.hardware_id); },
            stop_timeout_ns))
    {
        // It may still leave later; until a restart, it takes no work:
        // Kernel::tick schedules only CPUs its topology lists as online.
        static_cast<void>(kernel.topology().mark_halted(cpu));
        return Status::io_error("secondary CPU did not park");
    }
    static_cast<void>(kernel.topology().mark_offline(cpu));
    if (leave_faults[cpu].load(std::memory_order_relaxed))
        return Status::fault("leaving CPU still recorded a user entry or a section");
    return Status::success();
#else
    return Status::unsupported("this profile starts no secondary CPUs");
#endif
}

Status restart_secondary_cpu([[maybe_unused]] Kernel &kernel, [[maybe_unused]] CpuId cpu)
{
#if defined(OK_ARCH_SMP_START)
    if (auto status = hotplug_request(cpu); !status)
        return status;
    const HotplugTurn turn;
    if (!turn)
        return Status::busy("another CPU hotplug request is running");
    const u32 bit = u32{1} << cpu;
    auto &record = cpu_record(cpu);
    if ((online_mask() & bit) != 0 || state_of(cpu) != CpuState::offline || record.stack_top == 0 ||
        !arch::smp::parked(cpu, record.hardware_id))
        return Status::invalid_argument("CPU is not parked");
    // Same stack, same start path; the counters stay cumulative.
    record.ipi_pending.store(0, std::memory_order_relaxed);
    record.reported_id.store(~uptr{0}, std::memory_order_relaxed);
    set_state(cpu, CpuState::starting);
    if (auto status = arch::smp::start(cpu, record.hardware_id, record.stack_lower, record.stack_top); !status)
    {
        set_state(cpu, CpuState::offline);
        return status;
    }
    // Online once it ran C++ and reported its identity. It then rejoins the
    // mask (a running CPU joining needs no lock; only leaving does) and must
    // answer an IPI, with the kernel lock released: a CPU spinning for the
    // lock may not take IPIs.
    bool came_online = false, identified = false, delivered = false;
    static_cast<void>(wait_unlocked(
        [&] {
            if (state_of(cpu) != CpuState::online)
                return false;
            came_online = true;
            identified = record.reported_id.load(std::memory_order_acquire) == record.hardware_id;
            if (!identified)
                return true;
            detail::online.fetch_or(bit, std::memory_order_acq_rel);
            const auto before = record.ipis_received.load(std::memory_order_acquire);
            send_ipi(cpu, ipi_ping);
            delivered = wait_for(
                [&] {
                    interrupt::serve_local_interrupts();
                    return record.ipis_received.load(std::memory_order_acquire) != before;
                },
                ping_timeout_ns);
            return true;
        },
        start_timeout_ns));
    if (!identified)
    {
        // Never online, or not the CPU it was started as: it stays outside.
        set_state(cpu, came_online ? CpuState::halted : CpuState::offline);
        return Status::io_error("restarted CPU did not come online");
    }
    if (auto status = kernel.scheduler().online_cpu(cpu); !status)
        return status;
    static_cast<void>(kernel.topology().mark_starting(cpu));
    static_cast<void>(kernel.topology().mark_online(cpu));
    if (!delivered)
        return Status::io_error("restarted CPU did not answer its IPI");
    return Status::success();
#else
    return Status::unsupported("this profile starts no secondary CPUs");
#endif
}

HotplugStatistics hotplug_statistics() noexcept
{
#if defined(OK_SMP_HOTPLUG_TEST) && defined(OK_ARCH_SMP_START)
    // The CPU the test cycles, also in reports before its first pass.
    auto statistics = hotplug_stats;
    if (const u32 secondaries = started_mask() & ~u32{1}; secondaries != 0)
        statistics.cpu = static_cast<CpuId>(31 - __builtin_clz(secondaries));
    return statistics;
#else
    return {};
#endif
}

void hotplug_test_pass() noexcept
{
#if defined(OK_SMP_HOTPLUG_TEST) && defined(OK_ARCH_SMP_START)
    const u32 secondaries = started_mask() & ~u32{1};
    if (bringup_kernel == nullptr || secondaries == 0 || current_cpu() != 0 || !kernel_lock.owned_by_current_cpu() ||
        native_shutdown_pending())
        return;
    const auto cpu = static_cast<CpuId>(31 - __builtin_clz(secondaries));
    const auto now = arch::monotonic_nanoseconds();
    if (!now)
        return;
    if (hotplug_next == 0 || now.value() < hotplug_next)
    {
        if (hotplug_next == 0)
            hotplug_next = now.value() + hotplug_period_ns;
        return;
    }
    // A stop that timed out leaves the CPU stopping: no restart, a failure.
    bool cycled = true;
    if ((online_mask() & (u32{1} << cpu)) != 0)
        cycled = stop_secondary_cpu(*bringup_kernel, cpu).ok();
    cycled = state_of(cpu) == CpuState::offline && restart_secondary_cpu(*bringup_kernel, cpu).ok() && cycled;
    ++(cycled ? hotplug_stats.cycles : hotplug_stats.failures);
    const auto after = arch::monotonic_nanoseconds();
    hotplug_next = (after ? after.value() : now.value()) + hotplug_period_ns;
#endif
}

} // namespace ok::smp

extern "C" [[noreturn]] void ok_smp_secondary_main(ok::smp::CpuId cpu)
{
    using namespace ok::smp;
    auto &record = cpu_record(cpu);
    record.reported_id.store(static_cast<ok::uptr>(ok::arch::smp::current_hardware_id()), std::memory_order_release);
    // A CPU the boot CPU already gave up on stays out: it never serves threads
    // and stays outside the online mask that IPIs and shootdowns cover.
#if defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_1)
    auto starting = static_cast<ok::u8>(CpuState::starting);
    const bool admitted = record.state.compare_exchange_strong(starting, static_cast<ok::u8>(CpuState::online),
                                                               std::memory_order_acq_rel);
#elif !defined(OK_ARCH_SMP_START)
    // i386 has no CMPXCHG and runs UP (smp_stub starts no secondary), so
    // this entry is unreachable there; it must still link.
    const bool admitted = false;
#else
#error "a profile that starts secondary CPUs needs a byte compare-exchange"
#endif
    for (;;)
    {
#if defined(OK_ARCH_SMP_START)
        // Hotplug: between steps, so outside user code and holding nothing.
        if (admitted && record.state.load(std::memory_order_acquire) == static_cast<ok::u8>(CpuState::stopping))
            leave(cpu);
#endif
        if (admitted && (online_mask() & (ok::u32{1} << cpu)) != 0 && ok_smp_secondary_step != nullptr &&
            ok_smp_secondary_step(cpu))
            continue;
        ok::arch::smp::wait_local();
    }
}
