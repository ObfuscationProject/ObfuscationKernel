#include "ok/interrupt/platform.hpp"

#if defined(OK_KERNEL_FREESTANDING) && (defined(__x86_64__) || defined(__i386__))
#include "../arch/x86_common/pic8259.hpp"
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
#else
using PlatformIrqChip = ModelIrqChip;
#endif

constinit PlatformIrqChip chip{};
constinit IrqRouter<PlatformIrqChip> router{chip};
constinit IrqStatistics statistics{};
bool started{false};
volatile bool interrupted{false};

void halt_until_interrupt()
{
#if defined(OK_PLATFORM_IRQ_WAIT) && (defined(__x86_64__) || defined(__i386__))
    // STI's one-instruction shadow makes the enable and HLT atomic: an
    // interrupt already pending is taken at the HLT and ends it.
    asm volatile("sti; hlt; cli" : : : "memory");
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
