#pragma once

#include "ok/core/types.hpp"
#include "ok/interrupt/irq_chip.hpp"

#include <string_view>

namespace ok::arch::arm
{

// QEMU virt's GICv2 (distributor 0x08000000, CPU interface 0x08010000). Lines
// are interrupt IDs: SPIs 32..255 are bindable; SGIs and PPIs are not, and the
// physical timer PPI 30 stays enabled for the tick. The trap path or the idle
// wait claims an interrupt from GICC_IAR before the top half runs, so
// acknowledge() has nothing left to do and end_of_interrupt() writes GICC_EOIR.
class GicV2 final
{
  public:
    static constexpr uptr distributor = 0x08000000;
    static constexpr uptr cpu_interface = 0x08010000;
    static constexpr interrupt::IrqLine timer_ppi = 30;
    static constexpr u32 spurious_id = 1023;

    Status initialize()
    {
        for (usize word = 1; word < line_count() / 32; ++word)
            reg(distributor + 0x180 + 4 * word) = 0xffffffffu;
        byte(distributor + 0x400 + timer_ppi) = 0x80;
        reg(distributor + 0x100) = 1u << timer_ppi;
        reg(distributor) = reg(distributor) | 1u;
        reg(cpu_interface + 4) = 0xff;
        reg(cpu_interface) = reg(cpu_interface) | 1u;
        initialized_ = true;
        return Status::success();
    }
    Status configure(interrupt::IrqLine line, interrupt::IrqTrigger trigger)
    {
        if (auto status = admit(line); !status.ok())
            return status;
        const uptr config = distributor + 0xc00 + 4 * (line / 16);
        const u32 edge = 2u << (2 * (line % 16));
        reg(config) = trigger == interrupt::IrqTrigger::edge ? (reg(config) | edge) : (reg(config) & ~edge);
        byte(distributor + 0x800 + line) = 0x01; // CPU 0.
        byte(distributor + 0x400 + line) = 0xa0;
        return Status::success();
    }
    Status mask(interrupt::IrqLine line)
    {
        if (auto status = admit(line); !status.ok())
            return status;
        reg(distributor + 0x180 + 4 * (line / 32)) = 1u << (line % 32);
        return Status::success();
    }
    Status unmask(interrupt::IrqLine line)
    {
        if (auto status = admit(line); !status.ok())
            return status;
        reg(distributor + 0x100 + 4 * (line / 32)) = 1u << (line % 32);
        return Status::success();
    }
    Status acknowledge(interrupt::IrqLine line)
    {
        return admit(line);
    }
    Status end_of_interrupt(interrupt::IrqLine line)
    {
        if (!initialized_ || line >= line_count())
            return Status::invalid_argument("GIC interrupt ID out of range");
        complete(line);
        return Status::success();
    }
    [[nodiscard]] usize line_count() const
    {
        return 256;
    }
    [[nodiscard]] std::string_view name() const
    {
        return "qemu-virt-gicv2";
    }
    // Claims the highest-priority pending interrupt; spurious_id when none.
    static u32 claim()
    {
        return reg(cpu_interface + 0x0c);
    }
    static void complete(u32 acknowledge_value)
    {
        reg(cpu_interface + 0x10) = acknowledge_value;
    }

  private:
    static volatile u32 &reg(uptr address)
    {
        return *reinterpret_cast<volatile u32 *>(address);
    }
    static volatile u8 &byte(uptr address)
    {
        return *reinterpret_cast<volatile u8 *>(address);
    }
    [[nodiscard]] Status admit(interrupt::IrqLine line) const
    {
        if (!initialized_)
            return Status::not_initialized("GIC not initialized");
        if (line < 32 || line >= line_count())
            return Status::invalid_argument("GIC line is not a bindable SPI");
        return Status::success();
    }
    bool initialized_{false};
};
static_assert(interrupt::IrqChip<GicV2>);

} // namespace ok::arch::arm
