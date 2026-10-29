#include "ok/interrupt/platform.hpp"

#if defined(OK_KERNEL_FREESTANDING) && (defined(__x86_64__) || defined(__i386__))
#include "pic8259.hpp"
#define OK_PLATFORM_IRQ_WAIT 1
#elif defined(OK_KERNEL_FREESTANDING) && (defined(__arm__) || defined(__aarch64__))
#include "../arch/arm_common/gicv2.hpp"
#define OK_PLATFORM_IRQ_WAIT 1
#elif defined(OK_KERNEL_FREESTANDING) && defined(__riscv)
#include "../arch/riscv_common/plic.hpp"
#define OK_PLATFORM_IRQ_WAIT 1
#elif defined(OK_KERNEL_FREESTANDING) && defined(__loongarch64)
#include "../arch/loongarch64/eiointc.hpp"
#define OK_PLATFORM_IRQ_WAIT 1
#elif defined(OK_KERNEL_FREESTANDING) && defined(__mips__)
#include "pic8259.hpp"
#define OK_PLATFORM_IRQ_WAIT 1
extern "C" void ok_mips_idle(ok::u32 interrupt_mask);
#elif defined(OK_KERNEL_FREESTANDING) && (defined(__powerpc__) || defined(__PPC__))
#include "../arch/ppc/mpic.hpp"
#define OK_PLATFORM_IRQ_WAIT 1
extern "C" void ok_ppc_idle(ok::u32 msr);
#endif

namespace ok::interrupt
{
namespace
{
// Static dispatch: each profile names its controller here. Profiles without a
// controller driver keep the model chip and never idle on interrupts.
#if defined(OK_PLATFORM_IRQ_WAIT) && (defined(__x86_64__) || defined(__i386__))
using PlatformIrqChip = Pic8259<X86PortIo>;
#elif defined(OK_PLATFORM_IRQ_WAIT) && (defined(__arm__) || defined(__aarch64__))
using PlatformIrqChip = arch::arm::GicV2;
#elif defined(OK_PLATFORM_IRQ_WAIT) && defined(__riscv)
using PlatformIrqChip = arch::riscv::Plic;
#elif defined(OK_PLATFORM_IRQ_WAIT) && defined(__loongarch64)
using PlatformIrqChip = arch::loongarch::Eiointc;
#elif defined(OK_PLATFORM_IRQ_WAIT) && defined(__mips__)
// Malta maps ISA ports into the GT-64120 PCI I/O window at physical
// 0x18000000, reached uncached through KSEG1.
struct MaltaPortIo
{
#if defined(__mips64)
    static constexpr uptr window = 0xffffffffb8000000ull;
#else
    static constexpr uptr window = 0xb8000000;
#endif
    static u8 in(u16 port)
    {
        return *reinterpret_cast<volatile u8 *>(window + port);
    }
    static void out(u16 port, u8 value)
    {
        *reinterpret_cast<volatile u8 *>(window + port) = value;
    }
};
using PlatformIrqChip = Pic8259<MaltaPortIo, false>;
#elif defined(OK_PLATFORM_IRQ_WAIT) && (defined(__powerpc__) || defined(__PPC__))
using PlatformIrqChip = arch::ppc::Mpic;
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
#elif defined(OK_PLATFORM_IRQ_WAIT) && defined(__loongarch64)
    // IDLE wakes when ESTAT.IS & ECFG.LIE is nonzero whatever CRMD.IE says.
    // IE stays clear, so nothing traps and the wait claims from the EIOINTC.
    uptr crmd, frequency, ratio, index = 4;
    asm volatile("csrrd %0, 0x0" : "=r"(crmd));
    asm volatile("csrxchg $zero, %0, 0x0" : : "r"(uptr{4}) : "memory");
    asm volatile("cpucfg %0, %1" : "=r"(frequency) : "r"(index));
    index = 5;
    asm volatile("cpucfg %0, %1" : "=r"(ratio) : "r"(index));
    const uptr ticks = (ratio >> 16) ? frequency * (ratio & 0xffff) / (ratio >> 16) / 100 : 1000000;
    // CSRWR swaps, so each write goes through a scratch register.
    uptr sources = (uptr{1} << 11) | PlatformIrqChip::hwi0, compare = (ticks & ~uptr(3)) | 1;
    asm volatile("csrwr %0, 0x4" : "+r"(sources) : : "memory");
    asm volatile("csrwr %0, 0x41" : "+r"(compare) : : "memory");
    asm volatile("dbar 0\nidle 0" : : : "memory");
    uptr estat;
    asm volatile("csrrd %0, 0x5" : "=r"(estat));
    uptr zero = 0, one = 1, timer_only = uptr{1} << 11;
    asm volatile("csrwr %0, 0x41" : "+r"(zero) : : "memory");
    asm volatile("csrwr %0, 0x44" : "+r"(one) : : "memory");
    asm volatile("csrwr %0, 0x4" : "+r"(timer_only) : : "memory");
    if (estat & PlatformIrqChip::hwi0)
    {
        if (const u32 line = PlatformIrqChip::claim(); line < 32)
            ok_interrupt_top_half(static_cast<IrqLine>(line));
    }
    else if (estat & (uptr{1} << 11))
        ok_interrupt_tick();
    asm volatile("csrxchg %0, %1, 0x0" : : "r"(crmd & uptr{4}), "r"(uptr{4}) : "memory");
#elif defined(OK_PLATFORM_IRQ_WAIT) && defined(__mips__)
    // WAIT on these pre-R6 cores wakes only for an interrupt it could take, so
    // ok_mips_idle enables IP2 (the 8259) and IP7 (CP0 timer) with Status.IE.
    // The exception vector recognizes the idle window and resumes after WAIT
    // with IE clear; the interrupt itself is claimed here.
    u32 count;
    asm volatile("mfc0 %0, $9" : "=r"(count));
    const u32 compare = count + 1000000; // The backend's 10 ms user quantum.
    asm volatile("mtc0 %0, $11\n\tehb" : : "r"(compare) : "memory");
    ok_mips_idle(0x8400);
    u32 cause;
    asm volatile("mfc0 %0, $13" : "=r"(cause));
    const u32 zero = 0;
    asm volatile("mtc0 %0, $11\n\tehb" : : "r"(zero) : "memory"); // Writing Compare clears IP7.
    if (cause & 0x400)
    {
        if (const u32 line = PlatformIrqChip::poll(); line < 16)
            ok_interrupt_top_half(static_cast<IrqLine>(line));
    }
    else if (cause & 0x8000)
        ok_interrupt_tick();
#elif defined(OK_PLATFORM_IRQ_WAIT) && (defined(__powerpc__) || defined(__PPC__))
    // e500 dozes on MSR[WE] with HID0[DOZE]; QEMU's BookE model wakes only for
    // an interrupt MSR[EE] admits, so ok_ppc_idle sets EE for the window and the
    // vector returns past it with EE clear. The wait then claims from the MPIC.
    // A one-shot decrementer (400 MHz timebase, 10 ms) is the tick.
    u32 msr, hid0, tcr;
    asm volatile("mfmsr %0" : "=r"(msr));
    asm volatile("mfspr %0, 1008" : "=r"(hid0));
    asm volatile("mfspr %0, 340" : "=r"(tcr));
    const u32 doze = hid0 | 0x00800000u, dec = 4000000, die = 0x04000000, dis = 0x08000000;
    asm volatile("mtspr 1008, %0\n\tisync" : : "r"(doze) : "memory");
    asm volatile("mtspr 336, %0" : : "r"(dis) : "memory");
    asm volatile("mtspr 22, %0\n\tmtspr 340, %1\n\tisync" : : "r"(dec), "r"(die) : "memory");
    ok_ppc_idle(msr | 0x8000u | 0x00040000u);
    u32 tsr;
    asm volatile("mfspr %0, 336" : "=r"(tsr));
    asm volatile("mtspr 340, %0\n\tmtspr 336, %1\n\tmtspr 1008, %2\n\tisync" : : "r"(tcr), "r"(dis), "r"(hid0) : "memory");
    if (const u32 vector = PlatformIrqChip::claim(); vector < 256)
        ok_interrupt_top_half(static_cast<IrqLine>(vector));
    else if (tsr & dis)
        ok_interrupt_tick();
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

// Claims the controller's pending line from a user-mode interrupt trap on
// boards whose trap path does not claim it itself.
void claim_platform_irq()
{
#if defined(OK_PLATFORM_IRQ_WAIT) && defined(__mips__)
    if (!started)
        return;
    if (const u32 line = PlatformIrqChip::poll(); line < 16)
        ok_interrupt_top_half(static_cast<IrqLine>(line));
#elif defined(OK_PLATFORM_IRQ_WAIT) && (defined(__powerpc__) || defined(__PPC__))
    if (!started)
        return;
    if (const u32 vector = PlatformIrqChip::claim(); vector < 256)
        ok_interrupt_top_half(static_cast<IrqLine>(vector));
#endif
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
