#pragma once

#include "ok/core/types.hpp"
#include "ok/interrupt/irq_chip.hpp"

#include <string_view>

namespace ok::arch::ppc
{

// The ppce500 board's Freescale MPIC at CCSR + 0x40000 (big-endian registers,
// so ordinary loads and stores). Lines are MPIC source numbers; each source is
// programmed with its own number as the vector and routed to CPU 0. The
// current-CPU aliases at +0x80/+0xa0/+0xb0 give CTPR, IACK and EOI. The idle
// wait or the trap path claims a line with IACK before the top half runs.
class Mpic final
{
  public:
    static constexpr uptr base = 0xe0040000;
    static constexpr u32 spurious_vector = 0xffff;

    Status initialize()
    {
        reg(base + 0x10e0) = spurious_vector; // SVR.
        reg(base + 0x80) = 0;                 // CTPR: accept every priority above 0.
        initialized_ = true;
        return Status::success();
    }
    Status configure(interrupt::IrqLine line, interrupt::IrqTrigger trigger)
    {
        if (auto status = admit(line); !status.ok())
            return status;
        constexpr u32 masked = 1u << 31, polarity = 1u << 23, level = 1u << 22;
        // Keep the reset polarity (Freescale internal sources are active high).
        const u32 kept = reg(ivpr(line)) & polarity;
        reg(ivpr(line)) = masked | kept | (trigger == interrupt::IrqTrigger::level ? level : 0u) | (8u << 16) | line;
        reg(ivpr(line) + 0x10) = 1; // IDR: CPU 0.
        return Status::success();
    }
    Status mask(interrupt::IrqLine line)
    {
        if (auto status = admit(line); !status.ok())
            return status;
        reg(ivpr(line)) = reg(ivpr(line)) | (1u << 31);
        return Status::success();
    }
    Status unmask(interrupt::IrqLine line)
    {
        if (auto status = admit(line); !status.ok())
            return status;
        reg(ivpr(line)) = reg(ivpr(line)) & ~(1u << 31);
        return Status::success();
    }
    Status acknowledge(interrupt::IrqLine line)
    {
        return admit(line);
    }
    Status end_of_interrupt(interrupt::IrqLine line)
    {
        if (!initialized_ || line >= line_count())
            return Status::invalid_argument("MPIC source out of range");
        reg(base + 0xb0) = 0;
        return Status::success();
    }
    [[nodiscard]] usize line_count() const
    {
        return 256;
    }
    [[nodiscard]] std::string_view name() const
    {
        return "ppce500-fsl-mpic";
    }
    // IACK: the claimed source's vector (its line), or spurious_vector.
    static u32 claim()
    {
        return reg(base + 0xa0);
    }

  private:
    static constexpr uptr ivpr(interrupt::IrqLine line)
    {
        return base + 0x10000 + 0x20 * uptr{line};
    }
    static volatile u32 &reg(uptr address)
    {
        return *reinterpret_cast<volatile u32 *>(address);
    }
    [[nodiscard]] Status admit(interrupt::IrqLine line) const
    {
        if (!initialized_)
            return Status::not_initialized("MPIC not initialized");
        if (line >= line_count())
            return Status::invalid_argument("MPIC source out of range");
        return Status::success();
    }
    bool initialized_{false};
};
static_assert(interrupt::IrqChip<Mpic>);

} // namespace ok::arch::ppc
