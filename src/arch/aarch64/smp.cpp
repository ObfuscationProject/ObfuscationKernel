#include "ok/arch/smp.hpp"
#include "ok/boot/boot_memory.hpp"
#include "ok/boot/fdt.hpp"
#include "ok/smp/bringup.hpp"
#include "../arm_common/gicv2.hpp"
#include "secondary.hpp"

#include <array>

extern "C" void ok_aarch64_secondary_entry();
extern "C" [[noreturn]] void ok_aarch64_secondary_start(ok::smp::CpuId cpu);

// QEMU virt secondaries start through PSCI CPU_ON on the conduit the device
// tree's /psci node names; IPIs are GICv2 SGIs and each CPU's local timer is
// its banked physical timer (PPI 30).
namespace ok::arch::smp
{
namespace
{
using arm::GicV2;

constexpr u32 psci_version = 0x84000000;
constexpr u64 mpidr_affinity = 0xff00ffffff;
// The kernel's identity-mapped Normal RAM, where the device tree must lie.
constexpr uptr ram_begin = 0x40000000;
constexpr uptr ram_end = 0x80000000;
constexpr u8 no_interface = 0xff;

constinit boot::CpuStartMethod conduit{boot::CpuStartMethod::none};
constinit u32 cpu_on{};
constinit std::array<u8, ok::smp::max_cpus> interfaces{};
constinit std::array<aarch64::SecondaryStart, ok::smp::max_cpus> starts{};

// SMCCC: x0..x3 carry the call, x0 the result; x4..x17 may be clobbered.
template <boot::CpuStartMethod Conduit> i64 psci_call(u64 function, u64 first, u64 second, u64 third)
{
    register u64 x0 asm("x0") = function;
    register u64 x1 asm("x1") = first;
    register u64 x2 asm("x2") = second;
    register u64 x3 asm("x3") = third;
    if constexpr (Conduit == boot::CpuStartMethod::psci_smc)
        asm volatile("smc #0"
                     : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
                     :
                     : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17",
                       "memory");
    else
        asm volatile("hvc #0"
                     : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
                     :
                     : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17",
                       "memory");
    return static_cast<i32>(x0);
}

i64 psci(boot::CpuStartMethod method, u64 function, u64 first = 0, u64 second = 0, u64 third = 0)
{
    return method == boot::CpuStartMethod::psci_smc
               ? psci_call<boot::CpuStartMethod::psci_smc>(function, first, second, third)
               : psci_call<boot::CpuStartMethod::psci_hvc>(function, first, second, third);
}

// The interface number GICD_SGIR targets, read by the CPU it names.
u8 own_interface()
{
    const auto mask = GicV2::cpu_target_mask();
    return mask == 0 ? no_interface : static_cast<u8>(__builtin_ctz(mask));
}

void clean_to_coherency(const void *object, usize size)
{
    u64 cache_type;
    asm volatile("mrs %0, ctr_el0" : "=r"(cache_type));
    const usize line = usize{4} << ((cache_type >> 16) & 15);
    const auto begin = reinterpret_cast<uptr>(object) & ~(line - 1);
    for (auto address = begin; address < reinterpret_cast<uptr>(object) + size; address += line)
        asm volatile("dc cvac, %0" : : "r"(address) : "memory");
    asm volatile("dsb sy" : : : "memory");
}

} // namespace

Status discover(boot::CpuTopologyMap &map)
{
    const auto &memory = boot::kernel_boot_memory();
    if (memory.qualified == nullptr || memory.qualified->source().source.format != boot::FirmwareFormat::fdt)
        return Status::not_found("the boot handoff kept no device tree");
    const auto blob = memory.qualified->source().source_storage;
    if (blob.address < ram_begin || blob.address >= ram_end || blob.size > ram_end - blob.address)
        return Status::unsupported("the device tree lies outside the kernel's RAM mapping");
    boot::CpuTopologyMap parsed{};
    if (auto status = boot::parse_fdt_cpus(
            {reinterpret_cast<const std::byte *>(static_cast<uptr>(blob.address)), static_cast<usize>(blob.size)},
            boot::FdtCpuFlavor::arm, parsed);
        !status)
        return status;
    // A uniprocessor boot never calls the firmware. Otherwise the conduit must
    // answer PSCI 0.2 or later before any CPU is started through it.
    if (parsed.count > 1 && parsed.method != boot::CpuStartMethod::none && psci(parsed.method, psci_version) < 0x2)
    {
        parsed.method = boot::CpuStartMethod::none;
        parsed.up_reason = "psci-version-unsupported";
    }
    if (parsed.count > 1)
        boot::mark_boot_cpu(parsed, current_hardware_id());
    map = parsed;
    return Status::success();
}

std::string_view start_method_name(const boot::CpuTopologyMap &map) noexcept
{
    switch (map.method)
    {
    case boot::CpuStartMethod::psci_hvc:
        return "psci-hvc";
    case boot::CpuStartMethod::psci_smc:
        return "psci-smc";
    default:
        return "unsupported";
    }
}

Status prepare_boot_cpu(const boot::CpuTopologyMap &map)
{
    if (map.method != boot::CpuStartMethod::psci_hvc && map.method != boot::CpuStartMethod::psci_smc)
        return Status::unsupported("secondary CPUs start only through PSCI");
    interfaces.fill(no_interface);
    // execution::initialize() reset this CPU's banked lines; the IPI SGI stays on.
    GicV2::enable_cpu_lines();
    interfaces[0] = own_interface();
    if (interfaces[0] == no_interface)
        return Status::unsupported("the GIC names no CPU interface for the boot CPU");
    conduit = map.method;
    cpu_on = map.psci_cpu_on;
    return Status::success();
}

u64 current_hardware_id() noexcept
{
    u64 mpidr;
    asm volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    return mpidr & mpidr_affinity;
}

Status start(ok::smp::CpuId cpu, u64 hardware_id, uptr, uptr stack_top)
{
    if (cpu == 0 || cpu >= ok::smp::max_cpus || conduit == boot::CpuStartMethod::none)
        return Status::invalid_argument("no PSCI start path for this CPU");
    auto &start = starts[cpu];
    start = {};
    if (auto status = aarch64::prepare_secondary(cpu, start); !status)
        return status;
    start.stack_top = stack_top;
    start.entry = reinterpret_cast<uptr>(&ok_aarch64_secondary_start);
    start.cpu = cpu;
    // The new CPU reads its start block with the MMU and caches off.
    clean_to_coherency(&start, sizeof(start));
    const auto result = psci(conduit, cpu_on, hardware_id, reinterpret_cast<uptr>(&ok_aarch64_secondary_entry),
                             reinterpret_cast<uptr>(&start));
    if (result < 0)
        return Status::io_error("PSCI CPU_ON refused the CPU");
    return Status::success();
}

void send_ipi(ok::smp::CpuId cpu, u64) noexcept
{
    if (cpu >= ok::smp::max_cpus || interfaces[cpu] == no_interface)
        return;
    // The reasons stored in the target's record are visible before the SGI.
    asm volatile("dsb ishst" : : : "memory");
    GicV2::raise_ipi(u32{1} << interfaces[cpu]);
}

void wait_local() noexcept
{
    // Like the boot CPU's idle wait: WFI wakes on a pending IRQ with IRQs
    // masked, and the wait claims it from this CPU's interface. A one-shot
    // 100 Hz local timer bounds it.
    u64 frequency;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
    asm volatile("msr cntp_tval_el0, %0; msr cntp_ctl_el0, %1; isb" : : "r"(frequency / 100), "r"(u64{1}) : "memory");
    asm volatile("dsb sy; wfi" : : : "memory");
    const u32 acknowledge = GicV2::claim();
    const u32 id = acknowledge & 0x3ff;
    if (id == GicV2::spurious_id)
        return;
    asm volatile("msr cntp_ctl_el0, xzr; isb" : : : "memory");
    if (id < 16)
    {
        asm volatile("dmb ish" : : : "memory");
        GicV2::complete(acknowledge);
        static_cast<void>(ok::smp::receive_ipi());
    }
    else
    {
        if (id < 1020)
            GicV2::complete(acknowledge);
        if (id == GicV2::timer_ppi)
            ok::smp::local_tick();
    }
}

} // namespace ok::arch::smp

// The first C++ a secondary CPU runs, on its own stack with its MMU, caches,
// vectors and per-CPU block in place.
extern "C" [[noreturn]] void ok_aarch64_secondary_start(ok::smp::CpuId cpu)
{
    using namespace ok::arch::smp;
    GicV2::reset_cpu_lines();
    interfaces[cpu] = own_interface();
    asm volatile("msr cntp_ctl_el0, xzr; isb" : : : "memory");
    ok_smp_secondary_main(cpu);
}
