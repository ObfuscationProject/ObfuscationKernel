#pragma once

#include "ok/core/types.hpp"
#include "ok/interrupt/irq_chip.hpp"

#include <string_view>

namespace ok::arch::loongarch
{

// QEMU loongarch virt: PCH-PIC (MMIO 0x10000000) inputs 0..31 forward to the
// same-numbered EIOINTC vector, which the reset maps to core 0 and CPU
// interrupt pin IP0 (ESTAT HWI0). Lines are PCH-PIC inputs. Masking clears
// the EIOINTC enable bit, which also drops the line's core ISR; unmasking sets
// it again and re-delivers an input still asserted. The trap path or the idle
// wait claims a line from the core ISR before the top half runs, and
// end_of_interrupt() clears it there (and the PCH-PIC edge latch).
class Eiointc final
{
  public:
    static constexpr uptr pch_pic = 0x10000000;
    static constexpr u32 enable = 0x1600, core_isr = 0x1800; // IOCSR addresses.
    static constexpr u32 hwi0 = 1u << 2;                      // ESTAT/ECFG bit of IP0.

    Status initialize()
    {
        pch(0x20) = ~u64{0};  // INT_MASK: everything masked at the PCH-PIC.
        pch(0x3e0) = 0;       // INT_POL: active high.
        iocsr_write(enable, 0);
        iocsr_write(core_isr, ~u32{0});
        initialized_ = true;
        return Status::success();
    }
    Status configure(interrupt::IrqLine line, interrupt::IrqTrigger trigger)
    {
        if (auto status = admit(line); !status.ok())
            return status;
        const u64 bit = u64{1} << line;
        pch(0x60) = trigger == interrupt::IrqTrigger::edge ? (pch(0x60) | bit) : (pch(0x60) & ~bit);
        // HTMSI_VEC: one byte per input, written as the containing 64-bit word.
        const uptr word = 0x200 + (line & ~7u);
        const unsigned shift = 8 * (line & 7);
        pch(word) = (pch(word) & ~(u64{0xff} << shift)) | (u64{line} << shift);
        pch(0x20) = pch(0x20) & ~bit;
        return Status::success();
    }
    Status mask(interrupt::IrqLine line)
    {
        if (auto status = admit(line); !status.ok())
            return status;
        iocsr_write(enable, iocsr_read(enable) & ~(1u << line));
        return Status::success();
    }
    Status unmask(interrupt::IrqLine line)
    {
        if (auto status = admit(line); !status.ok())
            return status;
        iocsr_write(enable, iocsr_read(enable) | (1u << line));
        return Status::success();
    }
    Status acknowledge(interrupt::IrqLine line)
    {
        return admit(line);
    }
    Status end_of_interrupt(interrupt::IrqLine line)
    {
        if (!initialized_ || line >= line_count())
            return Status::invalid_argument("EIOINTC line out of range");
        iocsr_write(core_isr, 1u << line);
        pch(0x80) = u64{1} << line; // INT_CLEAR; ignored for level inputs.
        return Status::success();
    }
    [[nodiscard]] usize line_count() const
    {
        return 32;
    }
    [[nodiscard]] std::string_view name() const
    {
        return "qemu-virt-eiointc-pch-pic";
    }
    // The lowest line pending at core 0, or line_count() when none is.
    [[nodiscard]] static u32 claim()
    {
        const u32 pending = iocsr_read(core_isr);
        return pending == 0 ? 32 : static_cast<u32>(__builtin_ctz(pending));
    }

  private:
    static volatile u64 &pch(uptr offset)
    {
        return *reinterpret_cast<volatile u64 *>(pch_pic + offset);
    }
    static u32 iocsr_read(u32 address)
    {
        u32 value;
        asm volatile("iocsrrd.w %0, %1" : "=r"(value) : "r"(uptr{address}) : "memory");
        return value;
    }
    static void iocsr_write(u32 address, u32 value)
    {
        asm volatile("iocsrwr.w %0, %1" : : "r"(value), "r"(uptr{address}) : "memory");
    }
    [[nodiscard]] Status admit(interrupt::IrqLine line) const
    {
        if (!initialized_)
            return Status::not_initialized("EIOINTC not initialized");
        if (line >= line_count())
            return Status::invalid_argument("EIOINTC line out of range");
        return Status::success();
    }
    bool initialized_{false};
};
static_assert(interrupt::IrqChip<Eiointc>);

} // namespace ok::arch::loongarch
