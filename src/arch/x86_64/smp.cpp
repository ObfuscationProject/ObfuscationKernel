#include "ok/arch/smp.hpp"

#include "cpu_local.hpp"
#include "lapic.hpp"
#include "ok/arch/clock.hpp"
#include "ok/boot/acpi.hpp"
#include "ok/smp/bringup.hpp"
#include "ok/smp/cpu.hpp"

#include <cstddef>
#include <span>

extern "C"
{
    extern const ok::u8 ok_x86_smp_trampoline[];
    extern const ok::u8 ok_x86_smp_trampoline_parameters[];
    extern const ok::u8 ok_x86_smp_trampoline_end[];
    [[noreturn]] void ok_x86_smp_secondary_entry(ok::smp::CpuId cpu);
}

namespace ok::arch::smp
{
namespace
{
namespace lapic = x86::lapic;

// The boot loader's dead bounce buffer: the first MiB is never allocated, and
// the E820 table at 0x5000 ends below it.
constexpr uptr trampoline_base = 0x8000;
constexpr u64 identity_limit = 0x40000000;
constexpr u32 delivery_init_assert = 0xc500;
constexpr u32 delivery_init_deassert = 0x8500;
constexpr u32 delivery_startup = 0x0600;
constexpr u32 delivery_fixed = 0x4000;
constexpr u32 delivery_extint = 0x700;
constexpr u32 delivery_nmi = 0x400;
constexpr u32 apic_software_enable = 0x100;
constexpr u32 timer_divide_16 = 0x3;

// smp.S's parameter block.
struct TrampolineParameters
{
    u64 arrived;
    u64 cr0;
    u64 cr3;
    u64 cr4;
    u64 entry;
    u64 stack;
    u64 cpu;
};

u64 read_msr(u32 index)
{
    u32 low, high;
    asm volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(index));
    return (u64{high} << 32) | low;
}
u64 read_tsc()
{
    u32 low, high;
    asm volatile("rdtsc" : "=a"(low), "=d"(high) : : "memory");
    return (u64{high} << 32) | low;
}
void outw(u16 port, u16 value)
{
    asm volatile("outw %0, %1" : : "a"(value), "Nd"(port) : "memory");
}
u8 inb(u16 port)
{
    u8 value;
    asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}

// Busy-waits on the boot CPU's calibrated TSC.
void delay_ns(u64 nanoseconds)
{
    const u64 cycles = clock_frequency() / 1000 * nanoseconds / 1'000'000 + 1;
    const u64 start = read_tsc();
    while (read_tsc() - start < cycles)
        ArchOperations::spin_pause();
}

// Firmware tables are read through the boot identity map, which ends at 1 GiB.
std::span<const std::byte> identity_window(void *, u64 physical, usize size)
{
    if (physical == 0 || physical >= identity_limit || size > identity_limit - physical)
        return {};
    return {reinterpret_cast<const std::byte *>(static_cast<uptr>(physical)), size};
}

// QEMU's FW_CFG_NB_CPUS, or 0 without fw_cfg.
u16 fw_cfg_cpu_count()
{
    outw(0x510, 0x0000);
    if (inb(0x511) != 'Q' || inb(0x511) != 'E' || inb(0x511) != 'M' || inb(0x511) != 'U')
        return 0;
    outw(0x510, 0x0005);
    const u16 low = inb(0x511);
    return static_cast<u16>(low | (u16{inb(0x511)} << 8));
}

void send_icr(u64 apic_id, u32 command)
{
    while ((lapic::reg(lapic::icr_low) & lapic::delivery_pending) != 0)
        ArchOperations::spin_pause();
    lapic::reg(lapic::icr_high) = static_cast<u32>(apic_id) << 24;
    lapic::reg(lapic::icr_low) = command;
    while ((lapic::reg(lapic::icr_low) & lapic::delivery_pending) != 0)
        ArchOperations::spin_pause();
}

// Counts the divide-by-16 timer over 10 ms of TSC.
Status calibrate_timer()
{
    const u64 window = clock_frequency() / 100;
    if (window == 0)
        return Status::not_initialized("local APIC timer calibration needs the TSC clock");
    lapic::reg(lapic::timer_divide) = timer_divide_16;
    lapic::reg(lapic::lvt_timer) = lapic::lvt_masked | lapic::timer_vector;
    lapic::reg(lapic::timer_initial) = ~u32{0};
    const u64 start = read_tsc();
    while (read_tsc() - start < window)
        ArchOperations::spin_pause();
    const u32 remaining = lapic::reg(lapic::timer_current);
    lapic::reg(lapic::timer_initial) = 0;
    if (remaining == 0 || remaining == ~u32{0})
        return Status::io_error("local APIC timer did not count");
    lapic::timer_period = ~u32{0} - remaining;
    return Status::success();
}

// A secondary takes only its periodic local timer and IPIs.
void prepare_secondary_apic() noexcept
{
    lapic::reg(lapic::task_priority) = 0;
    lapic::reg(lapic::lvt_lint0) = lapic::lvt_masked | delivery_extint;
    lapic::reg(lapic::lvt_lint1) = lapic::lvt_masked | delivery_nmi;
    lapic::reg(lapic::lvt_error) = lapic::lvt_masked;
    lapic::reg(lapic::spurious) = apic_software_enable | lapic::spurious_vector;
    lapic::reg(lapic::timer_divide) = timer_divide_16;
    lapic::reg(lapic::lvt_timer) = lapic::timer_periodic | lapic::timer_vector;
    lapic::reg(lapic::timer_initial) = lapic::timer_period;
}

} // namespace

Status discover(boot::CpuTopologyMap &map)
{
    const auto up = [&](std::string_view reason) {
        map = {};
        map.count = 1;
        map.cpus[0] = {.hardware_id = current_hardware_id(), .enabled = true, .boot = true};
        map.boot_known = true;
        map.up_reason = reason;
        return Status::success();
    };
    const boot::PhysicalReader reader{&identity_window, nullptr};
    u64 rsdp = 0;
    if (!boot::find_acpi_rsdp(reader, rsdp))
        return up("acpi-absent");
    boot::CpuTopologyMap parsed{};
    if (auto status = boot::parse_acpi_madt(reader, rsdp, parsed); !status)
        return status;
    if (parsed.method != boot::CpuStartMethod::x86_sipi)
        return up(parsed.up_reason.empty() ? std::string_view{"single-cpu"} : parsed.up_reason);
    // Only the xAPIC page boot.S maps can start and interrupt CPUs.
    const u64 apic_base = read_msr(0x1b);
    if (parsed.local_apic_address != lapic::base || (apic_base & ~u64{0xfff}) != lapic::base ||
        (apic_base & (u64{1} << 11)) == 0 || (apic_base & (u64{1} << 10)) != 0)
        return up("xapic-unavailable");
    for (usize i = 0; i < parsed.count; ++i)
        if (parsed.cpus[i].enabled && parsed.cpus[i].hardware_id > 0xfe)
            return up("x2apic-id");
    // A sanity bound only: the MADT stays the description.
    if (const auto firmware = fw_cfg_cpu_count(); firmware != 0 && parsed.enabled_count() > firmware)
        return up("madt-exceeds-fw-cfg");
    static_cast<void>(boot::mark_boot_cpu(parsed, current_hardware_id()));
    map = parsed;
    return Status::success();
}

std::string_view start_method_name(const boot::CpuTopologyMap &map) noexcept
{
    return map.method == boot::CpuStartMethod::x86_sipi ? std::string_view{"sipi"} : std::string_view{"unsupported"};
}

Status prepare_boot_cpu(const boot::CpuTopologyMap &)
{
    execution::x86::ok_x86_cpu_locals[0].apic_id = current_hardware_id();
    // The 8259 keeps delivering through LINT0 as ExtINT, which needs no local
    // APIC EOI, so the PIT tick and device lines stay on this CPU.
    lapic::reg(lapic::task_priority) = 0;
    lapic::reg(lapic::lvt_timer) = lapic::lvt_masked | lapic::timer_vector;
    lapic::reg(lapic::lvt_error) = lapic::lvt_masked;
    lapic::reg(lapic::lvt_lint0) = delivery_extint;
    lapic::reg(lapic::lvt_lint1) = delivery_nmi;
    lapic::reg(lapic::spurious) = apic_software_enable | lapic::spurious_vector;
    return calibrate_timer();
}

u64 current_hardware_id() noexcept
{
    u32 a = 1, b, c, d;
    asm volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
    return b >> 24; // Initial APIC id.
}

Status start(ok::smp::CpuId cpu, u64 hardware_id, uptr, uptr stack_top)
{
    uptr cr0, cr3, cr4;
    asm volatile("mov %%cr0, %0; mov %%cr3, %1; mov %%cr4, %2" : "=r"(cr0), "=r"(cr3), "=r"(cr4));
    if (hardware_id > 0xfe || cpu == 0 || cpu >= ok::smp::max_cpus)
        return Status::invalid_argument("x86 secondary start needs an xAPIC id and a secondary slot");
    if ((cr3 >> 32) != 0)
        return Status::unsupported("the boot PML4 lies above the trampoline's 4 GiB reach");
    if (stack_top > identity_limit || (stack_top & 15) != 0)
        return Status::invalid_argument("x86 secondary stack lies outside the boot identity map");
    execution::x86::prepare_cpu(cpu, static_cast<u32>(hardware_id));

    const usize size = static_cast<usize>(ok_x86_smp_trampoline_end - ok_x86_smp_trampoline);
    auto *page = reinterpret_cast<volatile u8 *>(trampoline_base);
    for (usize i = 0; i < size; ++i)
        page[i] = ok_x86_smp_trampoline[i];
    auto &parameters = *reinterpret_cast<volatile TrampolineParameters *>(
        trampoline_base + static_cast<uptr>(ok_x86_smp_trampoline_parameters - ok_x86_smp_trampoline));
    parameters.arrived = 0;
    parameters.cr0 = cr0;
    parameters.cr3 = cr3;
    parameters.cr4 = cr4;
    parameters.entry = reinterpret_cast<uptr>(&ok_x86_smp_secondary_entry);
    parameters.stack = stack_top;
    parameters.cpu = cpu;
    asm volatile("mfence" : : : "memory");

    // INIT, then STARTUP at the trampoline page; a second STARTUP only if the
    // first was not taken.
    send_icr(hardware_id, delivery_init_assert);
    send_icr(hardware_id, delivery_init_deassert);
    delay_ns(10'000'000);
    constexpr u32 vector = trampoline_base >> 12;
    send_icr(hardware_id, delivery_startup | vector);
    delay_ns(200'000);
    if (parameters.arrived == 0)
    {
        send_icr(hardware_id, delivery_startup | vector);
        delay_ns(200'000);
    }
    return Status::success();
}

void send_ipi(ok::smp::CpuId, u64 hardware_id) noexcept
{
    // An interrupt on this CPU must not split the ICR's two writes.
    const auto saved = ArchOperations::save_and_disable_interrupts();
    send_icr(hardware_id, delivery_fixed | lapic::ipi_vector);
    ArchOperations::restore_interrupts(saved);
}

void wait_local() noexcept
{
    // The periodic local timer bounds the wait; its handler, or the IPI
    // handler, runs at the HLT.
    asm volatile("sti; hlt; cli" : : : "memory");
}

} // namespace ok::arch::smp

// The trampoline's 64-bit target, on the CPU's own stack and the boot PML4.
extern "C" [[noreturn]] void ok_x86_smp_secondary_entry(ok::smp::CpuId cpu)
{
    const ok::u32 mxcsr = 0x1f80;
    asm volatile("fninit; ldmxcsr %0" : : "m"(mxcsr) : "memory");
    ok::arch::execution::x86::load_cpu(cpu);
    ok::arch::smp::prepare_secondary_apic();
    ok_smp_secondary_main(cpu);
}
