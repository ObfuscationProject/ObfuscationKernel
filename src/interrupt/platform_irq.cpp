#include "ok/interrupt/platform.hpp"

#if defined(OK_KERNEL_FREESTANDING) && (defined(__x86_64__) || defined(__i386__))
#include "../arch/x86_common/pic8259.hpp"
#define OK_PLATFORM_IRQ_WAIT 1
#elif defined(OK_KERNEL_FREESTANDING) && (defined(__arm__) || defined(__aarch64__))
#include "../arch/arm_common/gicv2.hpp"
#define OK_PLATFORM_IRQ_WAIT 1
#elif defined(OK_KERNEL_FREESTANDING) && defined(__riscv)
#include "../arch/riscv_common/plic.hpp"
#define OK_PLATFORM_IRQ_WAIT 1
#endif

namespace ok::interrupt
{
namespace
{
// Static dispatch: each profile names its controller here. Profiles without a
// controller driver keep the model chip and never idle on interrupts.
#if defined(OK_PLATFORM_IRQ_WAIT) && (defined(__x86_64__) || defined(__i386__))
using PlatformIrqChip = arch::x86::Pic8259;
#elif defined(OK_PLATFORM_IRQ_WAIT) && (defined(__arm__) || defined(__aarch64__))
using PlatformIrqChip = arch::arm::GicV2;
#elif defined(OK_PLATFORM_IRQ_WAIT) && defined(__riscv)
using PlatformIrqChip = arch::riscv::Plic;
#else
using PlatformIrqChip = ModelIrqChip;
#endif

constinit PlatformIrqChip chip{};
constinit IrqRouter<PlatformIrqChip> router{chip};
constinit IrqStatistics statistics{};
bool started{false};
volatile bool interrupted{false};

#if defined(OK_PLATFORM_IRQ_WAIT) && defined(__arm__)
// The physical timer (PPI 30) as a one-shot idle tick at 100 Hz. The native
// backend reprograms it around each user entry and leaves it disabled.
void arm_idle_tick()
{
    u32 frequency;
    asm volatile("mrc p15, 0, %0, c14, c0, 0" : "=r"(frequency));
    const u32 ticks = frequency / 100, enable = 1;
    asm volatile("mcr p15, 0, %0, c14, c2, 0\nmcr p15, 0, %1, c14, c2, 1\nisb" ::"r"(ticks), "r"(enable) : "memory");
}
void disarm_idle_tick()
{
    const u32 disable = 0;
    asm volatile("mcr p15, 0, %0, c14, c2, 1\nisb" ::"r"(disable) : "memory");
}
#elif defined(OK_PLATFORM_IRQ_WAIT) && defined(__aarch64__)
void arm_idle_tick()
{
    u64 frequency;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
    asm volatile("msr cntp_tval_el0, %0; msr cntp_ctl_el0, %1; isb" : : "r"(frequency / 100), "r"(u64{1}) : "memory");
}
void disarm_idle_tick()
{
    asm volatile("msr cntp_ctl_el0, xzr; isb" : : : "memory");
}
#endif

#if defined(OK_PLATFORM_IRQ_WAIT) && defined(__riscv)
// The CLINT compare register as a one-shot idle tick: 100,000 ticks of the
// 10 MHz virt timebase. The native backend saves and restores it around each
// user entry; the wait leaves it at its maximum.
constexpr uptr clint_mtimecmp = 0x2004000, clint_mtime = 0x200bff8;
constexpr uptr mie_mtie = uptr{1} << 7, mie_meie = uptr{1} << 11;
void set_compare(u64 value)
{
#if __riscv_xlen == 32
    auto *compare = reinterpret_cast<volatile u32 *>(clint_mtimecmp);
    compare[0] = 0xffffffffu; // Never below the current time while the halves change.
    compare[1] = static_cast<u32>(value >> 32);
    compare[0] = static_cast<u32>(value);
#else
    *reinterpret_cast<volatile u64 *>(clint_mtimecmp) = value;
#endif
}
u64 machine_time()
{
#if __riscv_xlen == 32
    const auto *time = reinterpret_cast<volatile u32 *>(clint_mtime);
    u32 high, low;
    do
    {
        high = time[1];
        low = time[0];
    } while (time[1] != high);
    return (u64{high} << 32) | low;
#else
    return *reinterpret_cast<volatile u64 *>(clint_mtime);
#endif
}
#endif

void halt_until_interrupt()
{
#if defined(OK_PLATFORM_IRQ_WAIT) && (defined(__x86_64__) || defined(__i386__))
    // STI's one-instruction shadow makes the enable and HLT atomic: an
    // interrupt already pending is taken at the HLT and ends it.
    asm volatile("sti; hlt; cli" : : : "memory");
#elif defined(OK_PLATFORM_IRQ_WAIT) && (defined(__arm__) || defined(__aarch64__))
    // WFI wakes on a pending IRQ even with the IRQ mask set, so the kernel
    // waits masked and claims the interrupt from the GIC instead of taking it.
    arm_idle_tick();
    asm volatile("dsb sy\nwfi" : : : "memory");
    const u32 acknowledge = PlatformIrqChip::claim();
    const u32 id = acknowledge & 0x3ff;
    if (id == PlatformIrqChip::spurious_id)
        return;
    disarm_idle_tick();
    if (id == PlatformIrqChip::timer_ppi)
    {
        PlatformIrqChip::complete(acknowledge);
        ok_interrupt_tick();
    }
    else
        ok_interrupt_top_half(static_cast<IrqLine>(id));
#elif defined(OK_PLATFORM_IRQ_WAIT) && defined(__riscv)
    // In M-mode WFI wakes when mip & mie is nonzero whatever mstatus.MIE says;
    // MIE stays clear, so nothing traps and the wait claims from the PLIC.
    uptr status;
    asm volatile("csrrc %0, mstatus, %1" : "=r"(status) : "r"(uptr{8}) : "memory");
    set_compare(machine_time() + 100000);
    asm volatile("csrw mie, %0\nwfi\ncsrw mie, zero" : : "r"(mie_mtie | mie_meie) : "memory");
    uptr pending;
    asm volatile("csrr %0, mip" : "=r"(pending));
    set_compare(~u64{0});
    if (pending & mie_meie)
    {
        if (const u32 source = PlatformIrqChip::claim(); source != 0)
            ok_interrupt_top_half(static_cast<IrqLine>(source));
    }
    else if (pending & mie_mtie)
        ok_interrupt_tick();
    asm volatile("csrs mstatus, %0" : : "r"(status & uptr{8}) : "memory");
#endif
}
} // namespace

Status start_platform_irqs()
{
    if (started)
        return Status::success();
    if (auto status = chip.initialize(); !status.ok())
        return status;
    started = true;
    if (ok_platform_bind_irqs != nullptr)
        return ok_platform_bind_irqs();
    return Status::success();
}

bool platform_irqs_started()
{
    return started;
}

bool platform_irqs_supported()
{
#if defined(OK_PLATFORM_IRQ_WAIT)
    return true;
#else
    return false;
#endif
}

Status bind_platform_irq(IrqLine line, IrqTrigger trigger, IrqHandler handler, void *context)
{
    if (!started)
        return Status::not_initialized("platform interrupts are not started");
    return router.bind(line, trigger, handler, context);
}

Status unbind_platform_irq(IrqLine line)
{
    return router.unbind(line);
}

usize drain_platform_irqs()
{
    const usize serviced = router.drain();
    statistics.serviced += serviced;
    return serviced;
}

bool platform_irq_pending()
{
    return router.pending();
}

bool wait_for_interrupt()
{
    if (!started || !platform_irqs_supported())
        return false;
    if (router.pending())
        return true;
    interrupted = false;
    ++statistics.waits;
    // A tick or a device top half sets the flag; any other wake (an NMI)
    // halts again rather than returning to a loop with nothing to do.
    while (!interrupted)
        halt_until_interrupt();
    return true;
}

IrqStatistics platform_irq_statistics()
{
    IrqStatistics result = statistics;
    result.spurious = router.spurious();
    result.controller_errors = router.controller_errors();
    return result;
}

} // namespace ok::interrupt

extern "C" void ok_interrupt_top_half(ok::interrupt::IrqLine line)
{
    ok::interrupt::router.top_half(line);
    ++ok::interrupt::statistics.delivered;
    ok::interrupt::interrupted = true;
}

extern "C" void ok_interrupt_tick()
{
    ++ok::interrupt::statistics.ticks;
    ok::interrupt::interrupted = true;
}
