#pragma once

#include "ok/core/types.hpp"
#include "ok/interrupt/irq_chip.hpp"

#include <array>
#include <string_view>

namespace ok::arch::arm
{

// QEMU virt's GICv2 (distributor 0x08000000, CPU interface 0x08010000). Lines
// are interrupt IDs: SPIs 32..255 are bindable and each targets the one CPU
// route() names (GICD_ITARGETSR); SGIs and PPIs are not, and every CPU keeps
// its banked physical timer PPI 30 and the IPI SGI enabled. The trap path or
// the idle wait of the target CPU claims an interrupt from its GICC_IAR before
// the top half runs, so acknowledge() has nothing left to do and
// end_of_interrupt() writes that CPU's GICC_EOIR.
class GicV2 final
{
  public:
    static constexpr uptr distributor = 0x08000000;
    static constexpr uptr cpu_interface = 0x08010000;
    static constexpr interrupt::IrqLine timer_ppi = 30;
    static constexpr u32 ipi_sgi = 1;
    static constexpr u32 spurious_id = 1023;
    static constexpr u8 no_interface = 0xff;

    // The CPU interface number of each logical CPU, recorded by the CPU itself
    // (cpu_target_mask()). Logical CPU 0 defaults to interface 0, which is
    // also what a uniprocessor GIC ignores.
    static void set_cpu_interface(ok::smp::CpuId cpu, u8 interface_number)
    {
        if (cpu < ok::smp::max_cpus)
            interfaces[cpu] = interface_number;
    }
    [[nodiscard]] static u8 cpu_interface_number(ok::smp::CpuId cpu)
    {
        return cpu < ok::smp::max_cpus ? interfaces[cpu] : no_interface;
    }

    // The distributor, once on CPU 0; each CPU enables its own lines.
    Status initialize()
    {
        for (usize word = 1; word < line_count() / 32; ++word)
            reg(distributor + 0x180 + 4 * word) = 0xffffffffu;
        enable_distributor();
        enable_cpu_lines();
        initialized_ = true;
        return Status::success();
    }
    static void enable_distributor()
    {
        reg(distributor) = reg(distributor) | 1u;
    }
    // This CPU's banked view: the timer PPI and the IPI SGI at one priority,
    // every priority admitted, and its CPU interface on.
    static void enable_cpu_lines()
    {
        byte(distributor + 0x400 + timer_ppi) = 0x80;
        byte(distributor + 0x400 + ipi_sgi) = 0x80;
        reg(distributor + 0x100) = (1u << timer_ppi) | (1u << ipi_sgi);
        reg(cpu_interface + 4) = 0xff;
        reg(cpu_interface) = reg(cpu_interface) | 1u;
    }
    // Discards the banked SGI/PPI state this CPU started with, then enables
    // its lines.
    static void reset_cpu_lines()
    {
        reg(distributor + 0x180) = 0xffffffffu;
        reg(distributor + 0x280) = 0xffffffffu;
        for (usize word = 0; word < 4; ++word)
            reg(distributor + 0xf10 + 4 * word) = 0xffffffffu;
        reg(cpu_interface + 8) = 0;
        enable_cpu_lines();
    }
    // The interface number of the calling CPU: GICD_ITARGETSR0 is banked and
    // reads as that CPU's own target bit. Zero on a uniprocessor GIC.
    static u32 cpu_target_mask()
    {
        return byte(distributor + 0x800);
    }
    // Raises the IPI SGI on the CPU interfaces in `targets`. Stores before it
    // must already be visible (dsb ishst).
    static void raise_ipi(u32 targets)
    {
        reg(distributor + 0xf00) = ((targets & 0xffu) << 16) | ipi_sgi;
    }
    // GICD_ICFGR is a read-modify-write of 16 lines' fields: the router's
    // lock serializes it.
    Status configure(interrupt::IrqLine line, interrupt::IrqTrigger trigger)
    {
        if (auto status = admit(line); !status.ok())
            return status;
        const uptr config = distributor + 0xc00 + 4 * (line / 16);
        const u32 edge = 2u << (2 * (line % 16));
        reg(config) = trigger == interrupt::IrqTrigger::edge ? (reg(config) | edge) : (reg(config) & ~edge);
        byte(distributor + 0x400 + line) = 0xa0;
        return Status::success();
    }
    // One target CPU per line (one byte store): every other CPU's IAR never
    // sees it, so no CPU wakes to read a spurious 1023.
    Status route(interrupt::IrqLine line, ok::smp::CpuId cpu)
    {
        if (auto status = admit(line); !status.ok())
            return status;
        const u8 interface_number = cpu_interface_number(cpu);
        if (interface_number >= 8)
            return Status::invalid_argument("GIC has no CPU interface for this CPU");
        byte(distributor + 0x800 + line) = static_cast<u8>(1u << interface_number);
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
    static inline constinit std::array<u8, ok::smp::max_cpus> interfaces{};
    bool initialized_{false};
};
static_assert(interrupt::IrqChip<GicV2>);

} // namespace ok::arch::arm
