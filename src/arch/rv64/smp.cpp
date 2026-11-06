#include "ok/arch/smp.hpp"

#include "ok/boot/fdt.hpp"
#include "ok/boot/raw_handoff.hpp"
#include "ok/interrupt/platform.hpp"
#include "ok/smp/bringup.hpp"
#include "../riscv_common/clint.hpp"
#include "../riscv_common/plic.hpp"

#include <atomic>
#include <span>

// An HSM-shaped hart start inside the M-mode kernel (there is no SBI under
// -bios none): secondary harts park in boot.S until hart 0 fills their start
// mailbox entry and raises their CLINT machine software interrupt (MSIP), which
// is also the IPI line afterwards.
namespace ok::arch::smp
{
struct HartMailbox
{
    std::atomic<uptr> entry;
    uptr stack_top;
    uptr thread_pointer;
    uptr cpu;
};
static_assert(sizeof(HartMailbox) == 32);
} // namespace ok::arch::smp

// boot.S: one entry per hart id below 16 (smp::max_cpus), kept out of BSS.
extern "C" ok::arch::smp::HartMailbox ok_rv64_hart_mailbox[ok::smp::max_cpus];
// execution.cpp: the started hart's per-CPU block and its own backend setup.
extern "C" ok::uptr ok_rv64_cpu_block(ok::u32 cpu, ok::uptr hart) noexcept;
extern "C" void ok_rv64_secondary_execution() noexcept;

// A released hart arrives here from its park loop with sp, tp and a0 (the
// logical id) taken from its mailbox entry.
extern "C" [[noreturn]] void ok_rv64_secondary_entry(ok::uptr cpu)
{
    ok_rv64_secondary_execution();
    ok_smp_secondary_main(static_cast<ok::smp::CpuId>(cpu));
}

namespace ok::arch::smp
{
namespace
{
using riscv::Clint;

constexpr u32 fdt_magic = 0xd00dfeed;
constexpr usize fdt_header_bytes = 40;
// The idle wake, like the boot hart's: 10 ms of the 10 MHz virt timebase.
constexpr u64 local_tick_interval = 100000;

u32 be32(const std::byte *bytes)
{
    return (std::to_integer<u32>(bytes[0]) << 24) | (std::to_integer<u32>(bytes[1]) << 16) |
           (std::to_integer<u32>(bytes[2]) << 8) | std::to_integer<u32>(bytes[3]);
}
} // namespace

Status discover(boot::CpuTopologyMap &map)
{
    // The DTB stays reserved in boot memory and is reached untranslated.
    const auto &handoff = boot::raw_boot_handoff();
    const uptr address = handoff.captured ? handoff.arguments[1] : 0;
    if (address == 0 || (address & 7) != 0)
        return Status::not_found("no device tree in the boot handoff");
    const auto *blob = reinterpret_cast<const std::byte *>(address);
    const u32 size = be32(blob + 4);
    if (be32(blob) != fdt_magic || size < fdt_header_bytes || size > boot::max_fdt_bytes)
        return Status::invalid_argument("boot handoff device tree header invalid");
    if (auto status = boot::parse_fdt_cpus({blob, size}, boot::FdtCpuFlavor::riscv, map); !status)
        return status;
    static_cast<void>(boot::mark_boot_cpu(map, current_hardware_id()));
    return Status::success();
}

std::string_view start_method_name(const boot::CpuTopologyMap &) noexcept
{
    return "clint-msip";
}

std::string_view clock_sync_name() noexcept
{
    return {}; // Every hart reads the one CLINT mtime.
}

Status prepare_boot_cpu(const boot::CpuTopologyMap &map)
{
    if (map.method != boot::CpuStartMethod::riscv_msip)
        return Status::unsupported("harts are released through CLINT MSIP only");
    Clint::clear_software();
    return Status::success();
}

u64 current_hardware_id() noexcept
{
    return Clint::hart_id();
}

Status start(ok::smp::CpuId cpu, u64 hardware_id, uptr, uptr stack_top)
{
    if (hardware_id >= ok::smp::max_cpus)
        return Status::unsupported("hart id has no start mailbox entry");
    auto &mailbox = ok_rv64_hart_mailbox[hardware_id];
    if (mailbox.entry.load(std::memory_order_acquire) != 0)
        return Status::already_exists("hart was already released");
    mailbox.stack_top = stack_top;
    mailbox.thread_pointer = ok_rv64_cpu_block(cpu, hardware_id);
    mailbox.cpu = cpu;
    mailbox.entry.store(reinterpret_cast<uptr>(&ok_rv64_secondary_entry), std::memory_order_release);
    Clint::raise_software(hardware_id);
    return Status::success();
}

void send_ipi(ok::smp::CpuId, u64 hardware_id) noexcept
{
    Clint::raise_software(hardware_id);
}

// MIE stays clear: WFI wakes on mip & mie, and the wait reads mip itself. Once
// the interrupt service owns the PLIC, a device line routed to this hart also
// ends the wait and runs its top half.
void wait_local() noexcept
{
    using riscv::mie_meie, riscv::mie_msie, riscv::mie_mtie;
    const bool devices = ok::interrupt::platform_irqs_started();
    Clint::set_compare(Clint::time() + local_tick_interval);
    uptr pending;
    asm volatile("csrw mie, %1\nwfi\ncsrr %0, mip\ncsrw mie, zero"
                 : "=r"(pending)
                 : "r"(mie_mtie | mie_msie | (devices ? mie_meie : 0))
                 : "memory");
    Clint::set_compare(~u64{0});
    if (pending & mie_msie)
    {
        Clint::clear_software();
        static_cast<void>(ok::smp::receive_ipi());
    }
    if (devices && (pending & mie_meie))
        if (const u32 source = riscv::Plic::claim(); source != 0)
            ok_interrupt_top_half(static_cast<ok::interrupt::IrqLine>(source));
    if (pending & mie_mtie)
        ok::smp::local_tick();
}

} // namespace ok::arch::smp
