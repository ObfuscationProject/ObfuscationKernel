#pragma once

#include "ok/core/types.hpp"
#include "ok/interrupt/irq_chip.hpp"

#include <string_view>

namespace ok::arch::riscv
{

// QEMU virt's PLIC at 0x0c000000, used through hart 0's machine-mode context
// (context 0): the kernel runs in M-mode. Lines are PLIC source IDs 1..127;
// source 0 means "none". The gateway owns edge/level handling, so configure()
// only sets a priority. The trap path or the idle wait claims a source before
// the top half runs; end_of_interrupt() writes it back as the completion.
class Plic final
{
  public:
    static constexpr uptr base = 0x0c000000;
    static constexpr uptr enable = base + 0x2000;           // Context 0.
    static constexpr uptr threshold = base + 0x200000;      // Context 0.
    static constexpr uptr claim_complete = base + 0x200004; // Context 0.

    Status initialize()
    {
        for (usize word = 0; word < line_count() / 32; ++word)
            reg(enable + 4 * word) = 0;
        reg(threshold) = 0;
        initialized_ = true;
        return Status::success();
    }
    Status configure(interrupt::IrqLine line, interrupt::IrqTrigger)
    {
        if (auto status = admit(line); !status.ok())
            return status;
        reg(base + 4 * line) = 1;
        return Status::success();
    }
    Status mask(interrupt::IrqLine line)
    {
        if (auto status = admit(line); !status.ok())
            return status;
        reg(enable + 4 * (line / 32)) = reg(enable + 4 * (line / 32)) & ~(1u << (line % 32));
        return Status::success();
    }
    Status unmask(interrupt::IrqLine line)
    {
        if (auto status = admit(line); !status.ok())
            return status;
        reg(enable + 4 * (line / 32)) = reg(enable + 4 * (line / 32)) | (1u << (line % 32));
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
    // Claims the highest-priority pending source; 0 when none.
    static u32 claim()
    {
        return reg(claim_complete);
    }
    static void complete(u32 source)
    {
        reg(claim_complete) = source;
    }

  private:
    static volatile u32 &reg(uptr address)
    {
        return *reinterpret_cast<volatile u32 *>(address);
    }
    [[nodiscard]] Status admit(interrupt::IrqLine line) const
    {
        if (!initialized_)
            return Status::not_initialized("PLIC not initialized");
        if (line == 0 || line >= line_count())
            return Status::invalid_argument("PLIC source out of range");
        return Status::success();
    }
    bool initialized_{false};
};
static_assert(interrupt::IrqChip<Plic>);

} // namespace ok::arch::riscv
