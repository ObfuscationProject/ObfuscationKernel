#pragma once

#include "ok/core/types.hpp"
#include "ok/interrupt/irq_chip.hpp"

#include <concepts>
#include <string_view>

namespace ok::interrupt
{

// I/O access for the 8259 pair: x86 port instructions, or the PCI I/O window
// a board maps ISA ports into (Malta's GT-64120).
template <typename Io>
concept PortIo = requires(u16 port, u8 value) {
    { Io::in(port) } -> std::same_as<u8>;
    { Io::out(port, value) } -> std::same_as<void>;
};

// A cascaded 8259A pair (PC, Malta's PIIX4), remapped to vectors 32..47 (line
// N is vector 32 + N). Line 2 is the cascade. With PitTick, line 0 is the PIT
// tick; both stay unmasked and are never bound. A board with another tick
// (Malta's CP0 timer) leaves line 0 masked. Lines 3..15 other than 8 and 13 can
// be level-triggered through the PIIX ELCR registers. Boot CPU only.
template <PortIo Io, bool PitTick = true> class Pic8259 final
{
  public:
    static constexpr u8 vector_base = 32;
    static constexpr IrqLine tick_line = 0;
    static constexpr IrqLine cascade_line = 2;

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
        mask_ = 0xffff & ~((PitTick ? 1u << tick_line : 0u) | (1u << cascade_line));
        write_mask();
        initialized_ = true;
        return Status::success();
    }
    Status configure(IrqLine line, IrqTrigger trigger)
    {
        if (auto status = admit(line); !status.ok())
            return status;
        const bool level = trigger == IrqTrigger::level;
        if (level && (line == 0 || line == 1 || line == 8 || line == 13))
            return Status::invalid_argument("8259 line cannot be level-triggered");
        const u16 port = line < 8 ? 0x4d0 : 0x4d1;
        const u8 bit = static_cast<u8>(1u << (line & 7));
        const u8 old = inb(port);
        outb(port, level ? static_cast<u8>(old | bit) : static_cast<u8>(old & ~bit));
        return Status::success();
    }
    Status mask(IrqLine line)
    {
        if (auto status = admit(line); !status.ok())
            return status;
        mask_ |= static_cast<u16>(1u << line);
        write_mask(line);
        return Status::success();
    }
    Status unmask(IrqLine line)
    {
        if (auto status = admit(line); !status.ok())
            return status;
        mask_ &= static_cast<u16>(~(1u << line));
        write_mask(line);
        return Status::success();
    }
    // Rejects a spurious line 7 or 15: its in-service bit is clear.
    Status acknowledge(IrqLine line)
    {
        if (!initialized_ || line >= line_count())
            return Status::invalid_argument("8259 line out of range");
        if ((line & 7) == 7 && (in_service() & (1u << line)) == 0)
            return Status::not_found("spurious 8259 interrupt");
        return Status::success();
    }
    Status end_of_interrupt(IrqLine line)
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
    // OCW3 poll: claims the highest-priority request as an acknowledge would,
    // without an interrupt cycle; line_count() when nothing is requested.
    static u32 poll()
    {
        outb(0x20, 0x0c);
        const u8 master = inb(0x20);
        if ((master & 0x80) == 0)
            return 16;
        if ((master & 7) != cascade_line)
            return master & 7;
        outb(0xa0, 0x0c);
        const u8 slave = inb(0xa0);
        if (slave & 0x80)
            return 8u + (slave & 7);
        // The master acknowledged the cascade for a request the slave dropped
        // (a level line deasserted first): complete it, or the cascade's
        // in-service bit blocks every later slave interrupt.
        outb(0x20, 0x20);
        return 16u;
    }

  private:
    static void outb(u16 port, u8 value)
    {
        Io::out(port, value);
    }
    static u8 inb(u16 port)
    {
        return Io::in(port);
    }
    [[nodiscard]] Status admit(IrqLine line) const
    {
        if (!initialized_)
            return Status::not_initialized("8259 not initialized");
        if (line >= line_count() || (PitTick && line == tick_line) || line == cascade_line)
            return Status::invalid_argument("8259 line is reserved or out of range");
        return Status::success();
    }
    void write_mask()
    {
        outb(0x21, static_cast<u8>(mask_));
        outb(0xa1, static_cast<u8>(mask_ >> 8));
    }
    // Only the chip that holds the line: port writes are the expensive part of
    // a completion on emulated and virtualized machines.
    void write_mask(IrqLine line)
    {
        if (line < 8)
            outb(0x21, static_cast<u8>(mask_));
        else
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

// x86 port instructions.
struct X86PortIo
{
    static u8 in(u16 port)
    {
#if defined(__x86_64__) || defined(__i386__)
        u8 value;
        asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port) : "memory");
        return value;
#else
        static_cast<void>(port);
        return 0xff;
#endif
    }
    static void out(u16 port, u8 value)
    {
#if defined(__x86_64__) || defined(__i386__)
        asm volatile("outb %0, %1" : : "a"(value), "Nd"(port) : "memory");
#else
        static_cast<void>(port);
        static_cast<void>(value);
#endif
    }
};
static_assert(IrqChip<Pic8259<X86PortIo>>);

} // namespace ok::interrupt
