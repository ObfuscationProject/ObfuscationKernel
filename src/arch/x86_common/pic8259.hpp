#pragma once

#include "ok/core/types.hpp"
#include "ok/interrupt/irq_chip.hpp"

#include <string_view>

namespace ok::arch::x86
{

// The PC's cascaded 8259A pair, remapped to vectors 32..47 (line N is vector
// 32 + N). Line 0 is the PIT tick and line 2 the cascade; the platform keeps
// both unmasked and never binds them. Lines 3..15 other than 8 and 13 can be
// level-triggered through the PIIX ELCR registers. Boot CPU only.
class Pic8259 final
{
  public:
    static constexpr u8 vector_base = 32;
    static constexpr interrupt::IrqLine tick_line = 0;
    static constexpr interrupt::IrqLine cascade_line = 2;

    Status initialize()
    {
        // ICW1-4: edge-triggered cascade, vectors 32/40, slave on line 2, 8086 mode.
        outb(0x20, 0x11);
        outb(0xa0, 0x11);
        outb(0x21, vector_base);
        outb(0xa1, vector_base + 8);
        outb(0x21, 0x04);
        outb(0xa1, 0x02);
        outb(0x21, 0x01);
        outb(0xa1, 0x01);
        mask_ = 0xffff & ~((1u << tick_line) | (1u << cascade_line));
        write_mask();
        initialized_ = true;
        return Status::success();
    }
    Status configure(interrupt::IrqLine line, interrupt::IrqTrigger trigger)
    {
        if (auto status = admit(line); !status.ok())
            return status;
        const bool level = trigger == interrupt::IrqTrigger::level;
        if (level && (line == 0 || line == 1 || line == 8 || line == 13))
            return Status::invalid_argument("8259 line cannot be level-triggered");
        const u16 port = line < 8 ? 0x4d0 : 0x4d1;
        const u8 bit = static_cast<u8>(1u << (line & 7));
        const u8 old = inb(port);
        outb(port, level ? static_cast<u8>(old | bit) : static_cast<u8>(old & ~bit));
        return Status::success();
    }
    Status mask(interrupt::IrqLine line)
    {
        if (auto status = admit(line); !status.ok())
            return status;
        mask_ |= static_cast<u16>(1u << line);
        write_mask();
        return Status::success();
    }
    Status unmask(interrupt::IrqLine line)
    {
        if (auto status = admit(line); !status.ok())
            return status;
        mask_ &= static_cast<u16>(~(1u << line));
        write_mask();
        return Status::success();
    }
    // Rejects a spurious line 7 or 15: its in-service bit is clear.
    Status acknowledge(interrupt::IrqLine line)
    {
        if (!initialized_ || line >= line_count())
            return Status::invalid_argument("8259 line out of range");
        if ((line & 7) == 7 && (in_service() & (1u << line)) == 0)
            return Status::not_found("spurious 8259 interrupt");
        return Status::success();
    }
    Status end_of_interrupt(interrupt::IrqLine line)
    {
        if (!initialized_ || line >= line_count())
            return Status::invalid_argument("8259 line out of range");
        if ((line & 7) == 7 && (in_service() & (1u << line)) == 0)
        {
            // A spurious slave interrupt still occupied the master's cascade line.
            if (line == 15)
                outb(0x20, 0x20);
            return Status::success();
        }
        if (line >= 8)
            outb(0xa0, 0x20);
        outb(0x20, 0x20);
        return Status::success();
    }
    [[nodiscard]] usize line_count() const
    {
        return 16;
    }
    [[nodiscard]] std::string_view name() const
    {
        return "pc-8259a-pair";
    }
    [[nodiscard]] u16 mask_state() const
    {
        return mask_;
    }

  private:
    static void outb(u16 port, u8 value)
    {
        asm volatile("outb %0, %1" : : "a"(value), "Nd"(port) : "memory");
    }
    static u8 inb(u16 port)
    {
        u8 value;
        asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port) : "memory");
        return value;
    }
    [[nodiscard]] Status admit(interrupt::IrqLine line) const
    {
        if (!initialized_)
            return Status::not_initialized("8259 not initialized");
        if (line >= line_count() || line == tick_line || line == cascade_line)
            return Status::invalid_argument("8259 line is reserved or out of range");
        return Status::success();
    }
    void write_mask()
    {
        outb(0x21, static_cast<u8>(mask_));
        outb(0xa1, static_cast<u8>(mask_ >> 8));
    }
    static u16 in_service()
    {
        outb(0x20, 0x0b);
        outb(0xa0, 0x0b);
        return static_cast<u16>(inb(0x20) | (inb(0xa0) << 8));
    }
    u16 mask_{0xffff};
    bool initialized_{false};
};
static_assert(interrupt::IrqChip<Pic8259>);

} // namespace ok::arch::x86
