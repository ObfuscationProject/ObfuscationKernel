#include "ok/smp/bringup.hpp"

#include "ok/arch/arch.hpp"
#include "ok/arch/clock.hpp"
#include "ok/arch/execution.hpp"
#include "ok/arch/smp.hpp"
#include "ok/interrupt/platform.hpp"
#include "ok/core/kernel.hpp"
#include "ok/memory/memory.hpp"
#include "ok/smp/cpu.hpp"
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
    detail::boot_wake = []() noexcept { ok_interrupt_wake(); };

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
    return Status::success();
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
#else
    // i386 has no CMPXCHG and runs UP (smp_stub starts no secondary), so
    // this entry is unreachable there; it must still link.
    const bool admitted = false;
#endif
    for (;;)
    {
        if (admitted && (online_mask() & (ok::u32{1} << cpu)) != 0 && ok_smp_secondary_step != nullptr &&
            ok_smp_secondary_step(cpu))
            continue;
        ok::arch::smp::wait_local();
    }
}
