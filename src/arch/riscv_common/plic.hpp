#pragma once

#include "ok/core/types.hpp"
#include "ok/interrupt/irq_chip.hpp"
#include "ok/smp/bringup.hpp"

#include <array>
#include <string_view>

namespace ok::arch::riscv
{

// QEMU virt's PLIC at 0x0c000000. The kernel runs in M-mode and QEMU gives
// each hart an M-mode and an S-mode context, so hart h uses context 2h; rv32
// runs hart 0 alone, through context 0. Lines are PLIC source IDs 1..127;
// source 0 means "none". The gateway owns edge/level handling, so configure()
// only admits the source. A source is enabled in one context only, the one
// its route() target uses: the trap path or the idle wait of that hart claims
// it before the top half runs, and end_of_interrupt() completes it through the
// same hart's context. Masking writes the source's priority to 0 ("never
// interrupt") and leaves the enable bit alone: the PLIC ignores a completion
// for a source not enabled in the completing context, and the top half masks
// a line before it completes it.
class Plic final
{
  public:
    static constexpr uptr base = 0x0c000000;

    [[nodiscard]] static constexpr u8 hart_context(uptr hart)
    {
#if __riscv_xlen == 32
        static_cast<void>(hart);
        return 0;
#else
        return static_cast<u8>(2 * hart);
#endif
    }
    [[nodiscard]] static constexpr uptr enable(usize context)
    {
        return base + 0x2000 + 0x80 * context;
    }
    [[nodiscard]] static constexpr uptr threshold(usize context)
    {
        return base + 0x200000 + 0x1000 * context;
    }
    [[nodiscard]] static constexpr uptr priority(interrupt::IrqLine line)
    {
        return base + 4 * line;
    }
    [[nodiscard]] static constexpr uptr claim_complete(usize context)
    {
        return base + 0x200004 + 0x1000 * context;
    }

    // Every started hart's context: nothing enabled, every priority admitted.
    Status initialize()
    {
        const u32 online = ok::smp::online_mask() | 1u;
        for (ok::smp::CpuId cpu = 0; cpu < ok::smp::max_cpus; ++cpu)
        {
            if ((online & (u32{1} << cpu)) == 0)
                continue;
            const auto context = cpu_context(cpu);
            for (usize word = 0; word < line_count() / 32; ++word)
                reg(enable(context) + 4 * word) = 0;
            reg(threshold(context)) = 0;
        }
        contexts_.fill(cpu_context(0));
        initialized_ = true;
        return Status::success();
    }
    Status configure(interrupt::IrqLine line, interrupt::IrqTrigger)
    {
        if (auto status = admit(line); !status.ok())
            return status;
        reg(priority(line)) = 0;
        return Status::success();
    }
    // One store, so a masked source keeps its enable bit for the completion.
    Status mask(interrupt::IrqLine line)
    {
        if (auto status = admit(line); !status.ok())
            return status;
        reg(priority(line)) = 0;
        return Status::success();
    }
    // QEMU's PLIC re-evaluates its interrupt outputs on a threshold write but
    // not on an enable write. A source that became pending while masked, from
    // a device that will not raise its line again (a 16550 holding one byte),
    // would otherwise never reach the hart, so unmasking rewrites the
    // threshold of the context it enabled. The enable words are
    // read-modify-write: the router's lock serializes them.
    Status unmask(interrupt::IrqLine line)
    {
        if (auto status = admit(line); !status.ok())
            return status;
        const auto context = contexts_[line];
        const uptr word = enable(context) + 4 * (line / 32);
        reg(word) = reg(word) | (1u << (line % 32));
        reg(priority(line)) = 1;
        reg(threshold(context)) = 0;
        return Status::success();
    }
    // Moves the source's enable bit to `cpu`'s context; the priority keeps
    // its masked state. Callers move a line only after its claim completed
    // (the top half completes before the router moves it), because a
    // completion through a context the source left is ignored.
    Status route(interrupt::IrqLine line, ok::smp::CpuId cpu)
    {
        if (auto status = admit(line); !status.ok())
            return status;
        if (cpu >= ok::smp::max_cpus || (cpu != 0 && (ok::smp::online_mask() & (u32{1} << cpu)) == 0))
            return Status::invalid_argument("PLIC route names an offline hart");
        const auto next = cpu_context(cpu), previous = contexts_[line];
        if (next == previous)
            return Status::success();
        const u32 bit = 1u << (line % 32);
        const uptr old_word = enable(previous) + 4 * (line / 32), new_word = enable(next) + 4 * (line / 32);
        if ((reg(old_word) & bit) != 0)
        {
            reg(old_word) = reg(old_word) & ~bit;
            reg(new_word) = reg(new_word) | bit;
            reg(threshold(next)) = 0;
        }
        contexts_[line] = next;
        return Status::success();
    }
    Status acknowledge(interrupt::IrqLine line)
    {
        return admit(line);
    }
    Status end_of_interrupt(interrupt::IrqLine line)
    {
        if (!initialized_ || line == 0 || line >= line_count())
            return Status::invalid_argument("PLIC source out of range");
        complete(line);
        return Status::success();
    }
    [[nodiscard]] usize line_count() const
    {
        return 128;
    }
    [[nodiscard]] std::string_view name() const
    {
        return "qemu-virt-plic";
    }
    // Claims the highest-priority source pending for this hart; 0 when none.
    static u32 claim()
    {
        return reg(claim_complete(current_context()));
    }
    static void complete(u32 source)
    {
        reg(claim_complete(current_context())) = source;
    }

  private:
    static volatile u32 &reg(uptr address)
    {
        return *reinterpret_cast<volatile u32 *>(address);
    }
    [[nodiscard]] static u8 current_context()
    {
#if __riscv_xlen == 32
        return 0;
#else
        uptr hart;
        asm volatile("csrr %0, mhartid" : "=r"(hart));
        return hart_context(hart);
#endif
    }
    [[nodiscard]] static u8 cpu_context(ok::smp::CpuId cpu)
    {
        return hart_context(static_cast<uptr>(ok::smp::cpu_record(cpu).hardware_id));
    }
    [[nodiscard]] Status admit(interrupt::IrqLine line) const
    {
        if (!initialized_)
            return Status::not_initialized("PLIC not initialized");
        if (line == 0 || line >= line_count())
            return Status::invalid_argument("PLIC source out of range");
        return Status::success();
    }
    // The context each source is enabled in (when unmasked).
    std::array<u8, 128> contexts_{};
    bool initialized_{false};
};
static_assert(interrupt::IrqChip<Plic> && interrupt::RoutableIrqChip<Plic>);

} // namespace ok::arch::riscv
