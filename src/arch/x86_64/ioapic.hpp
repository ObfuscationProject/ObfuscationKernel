#pragma once

#include "lapic.hpp"
#include "ok/boot/acpi.hpp"
#include "ok/interrupt/irq_chip.hpp"
#include "ok/smp/bringup.hpp"
#include "../../interrupt/pic8259.hpp"

#include <array>
#include <atomic>
#include <string_view>

namespace ok::arch::x86
{

// What the x86_64 trap paths need to know about platform lines. Vectors
// 32..63 carry them: the 8259 pair's lines 0..15 or, once the interrupt
// service routes through the I/O APIC, GSIs 0..31 (line N is vector 32 + N
// either way). The PIT tick is 8259 line 0, or the GSI the MADT moves ISA IRQ
// 0 to, and reaches the boot CPU only.
namespace ioapic
{
inline constexpr u32 vector_base = 32;
// execution.S has an IDT stub for each vector 32..63.
inline constexpr usize max_lines = 32;

// The MADT's interrupt description, recorded by smp::discover() when the
// local APIC is the xAPIC page boot.S maps; otherwise the 8259 pair stays.
inline constinit boot::MadtInterrupts firmware{};
inline constinit bool described{};
// Set by the boot CPU before it unmasks any I/O APIC entry; the GSI of the
// tick is written before it.
inline constinit std::atomic<bool> routing{};
inline constinit u32 tick_gsi{};

[[nodiscard]] inline bool active() noexcept
{
    return routing.load(std::memory_order_acquire);
}
[[nodiscard]] inline u32 tick_vector() noexcept
{
    return vector_base + (active() ? tick_gsi : 0);
}
} // namespace ioapic

// The I/O APIC whose GSIs start at 0, in physical destination mode with fixed
// delivery: line N is GSI N on vector 32 + N, delivered to the local APIC of
// the CPU route() names, which completes it with a local APIC EOI (that also
// clears a level line's Remote IRR). Trigger mode comes from the binding,
// polarity from the MADT (an override, else ISA active high or PCI active
// low). Its registers are an index/data pair: the router's lock serializes
// every access after initialize().
//
// The I/O APIC drops an edge that arrives while its entry is masked (QEMU
// follows the 82093AA there), so the router leaves edge lines unmasked in the
// top half (loses_masked_edges()): an edge during the bottom half marks the
// line pending again and its handlers run once more.
class IoApic final
{
  public:
    // boot.S maps this 2 MiB uncached at the same address.
    static constexpr uptr window_begin = 0xfec00000;
    static constexpr uptr window_end = 0xfee00000;

    // Masks every entry. Fails when the MADT names no I/O APIC with GSI base 0
    // inside the mapped window.
    Status initialize()
    {
        const boot::IoApicDescription *chosen = nullptr;
        for (usize i = 0; i < ioapic::firmware.io_apic_count; ++i)
            if (ioapic::firmware.io_apics[i].gsi_base == 0)
                chosen = &ioapic::firmware.io_apics[i];
        if (!ioapic::described || chosen == nullptr)
            return Status::not_found("the MADT describes no I/O APIC for GSI 0");
        if (chosen->address < window_begin || chosen->address > window_end - 0x20 || (chosen->address & 0xf) != 0)
            return Status::unsupported("the I/O APIC lies outside the mapped window");
        base_ = static_cast<uptr>(chosen->address);
        const u32 version = read(version_register);
        const usize entries = ((version >> 16) & 0xff) + 1;
        if ((version & 0xff) < 0x10 || entries < 16)
            return Status::unsupported("the I/O APIC register window does not answer as an I/O APIC");
        lines_ = entries < ioapic::max_lines ? entries : ioapic::max_lines;
        for (usize line = 0; line < lines_; ++line)
        {
            low_[line] = masked | (ioapic::vector_base + static_cast<u32>(line));
            write(redirection(line) + 1, 0);
            write(redirection(line), low_[line]);
        }
        boot_apic_id_ = lapic::reg(lapic::id) >> 24;
        return Status::success();
    }
    // Delivers `gsi` (below line_count()) to the boot CPU, unmasked, as its
    // wiring says: the tick.
    void start_tick(u32 gsi)
    {
        const auto wiring = ioapic::firmware.gsi(gsi);
        low_[gsi] = entry(gsi, wiring.level, wiring.active_low);
        write(redirection(gsi) + 1, boot_apic_id_ << 24);
        write(redirection(gsi), low_[gsi]);
    }
    Status configure(interrupt::IrqLine line, interrupt::IrqTrigger trigger)
    {
        if (auto status = admit(line); !status.ok())
            return status;
        const auto wiring = ioapic::firmware.gsi(line);
        low_[line] = entry(line, trigger == interrupt::IrqTrigger::level, wiring.active_low) | (low_[line] & masked);
        write(redirection(line), low_[line]);
        return Status::success();
    }
    Status mask(interrupt::IrqLine line)
    {
        if (auto status = admit(line); !status.ok())
            return status;
        low_[line] |= masked;
        write(redirection(line), low_[line]);
        return Status::success();
    }
    // QEMU re-evaluates an entry on every write, so a level line still
    // asserted is delivered again here.
    Status unmask(interrupt::IrqLine line)
    {
        if (auto status = admit(line); !status.ok())
            return status;
        low_[line] &= ~masked;
        write(redirection(line), low_[line]);
        return Status::success();
    }
    // A CPU this I/O APIC can name: the boot CPU or an online one.
    Status route(interrupt::IrqLine line, ok::smp::CpuId cpu)
    {
        if (auto status = admit(line); !status.ok())
            return status;
        if (cpu >= ok::smp::max_cpus || (cpu != 0 && (ok::smp::online_mask() & (u32{1} << cpu)) == 0))
            return Status::invalid_argument("I/O APIC route names an offline CPU");
        const u64 apic_id = cpu == 0 ? boot_apic_id_ : ok::smp::cpu_record(cpu).hardware_id;
        if (apic_id > 0xfe)
            return Status::unsupported("I/O APIC physical destinations are 8-bit xAPIC ids");
        write(redirection(line) + 1, static_cast<u32>(apic_id) << 24);
        return Status::success();
    }
    // The trap path took the vector from this CPU's local APIC already.
    Status acknowledge(interrupt::IrqLine line)
    {
        return line < lines_ ? Status::success() : Status::invalid_argument("I/O APIC line out of range");
    }
    // The CPU that took the vector completes it at its own local APIC.
    Status end_of_interrupt(interrupt::IrqLine line)
    {
        if (line >= lines_)
            return Status::invalid_argument("I/O APIC line out of range");
        lapic::end_of_interrupt();
        return Status::success();
    }
    [[nodiscard]] usize line_count() const
    {
        return lines_;
    }
    [[nodiscard]] std::string_view name() const
    {
        return "x86-ioapic";
    }

  private:
    static constexpr u32 version_register = 0x01;
    static constexpr u32 masked = 1u << 16;
    static constexpr u32 level_triggered = 1u << 15;
    static constexpr u32 active_low = 1u << 13;

    static constexpr u32 redirection(usize line)
    {
        return 0x10 + 2 * static_cast<u32>(line);
    }
    // Fixed delivery, physical destination; the mask bit is the caller's.
    static constexpr u32 entry(usize line, bool level, bool low)
    {
        return (ioapic::vector_base + static_cast<u32>(line)) | (level ? level_triggered : 0) | (low ? active_low : 0);
    }
    [[nodiscard]] u32 read(u32 index) const
    {
        *reinterpret_cast<volatile u32 *>(base_) = index;
        return *reinterpret_cast<volatile u32 *>(base_ + 0x10);
    }
    void write(u32 index, u32 value) const
    {
        *reinterpret_cast<volatile u32 *>(base_) = index;
        *reinterpret_cast<volatile u32 *>(base_ + 0x10) = value;
    }
    // The tick's GSI is never bound.
    [[nodiscard]] Status admit(interrupt::IrqLine line) const
    {
        if (lines_ == 0)
            return Status::not_initialized("I/O APIC not initialized");
        if (line >= lines_ || (ioapic::active() && line == ioapic::tick_gsi))
            return Status::invalid_argument("I/O APIC line is reserved or out of range");
        return Status::success();
    }

    uptr base_{};
    usize lines_{};
    u32 boot_apic_id_{};
    std::array<u32, ioapic::max_lines> low_{};
};
static_assert(interrupt::RoutableIrqChip<IoApic>);

// x86_64's platform controller: the I/O APIC the MADT describes, which can
// deliver every line to any CPU, or the 8259 pair on the boot CPU when there
// is none. initialize() (boot CPU, interrupts masked) picks one for good.
// Switching to the I/O APIC masks both 8259s and the boot CPU's LINT0, so no
// ExtINT arrives any more, enables the boot CPU's local APIC (uniprocessor
// boots have not) and moves the PIT tick to its GSI on the boot CPU.
class PcIrqChip final
{
  public:
    Status initialize()
    {
        if (!io_apic_.initialize().ok() || ioapic::firmware.isa_irq(0).gsi >= io_apic_.line_count())
            return pic_.initialize();
        // The boot CPU accepts fixed interrupts at any priority; LINT1 stays
        // NMI and LINT0 (the 8259's ExtINT) is masked.
        lapic::reg(lapic::task_priority) = 0;
        lapic::reg(lapic::lvt_error) = lapic::lvt_masked;
        lapic::reg(lapic::lvt_lint0) = lapic::lvt_masked | lapic::delivery_extint;
        lapic::reg(lapic::lvt_lint1) = lapic::delivery_nmi;
        lapic::reg(lapic::spurious) = lapic::software_enable | lapic::spurious_vector;
        interrupt::X86PortIo::out(0x21, 0xff);
        interrupt::X86PortIo::out(0xa1, 0xff);
        ioapic::tick_gsi = ioapic::firmware.isa_irq(0).gsi;
        ioapic::routing.store(true, std::memory_order_release);
        io_apic_.start_tick(ioapic::tick_gsi);
        apic_ = true;
        return Status::success();
    }
    Status configure(interrupt::IrqLine line, interrupt::IrqTrigger trigger)
    {
        return apic_ ? io_apic_.configure(line, trigger) : pic_.configure(line, trigger);
    }
    Status mask(interrupt::IrqLine line)
    {
        return apic_ ? io_apic_.mask(line) : pic_.mask(line);
    }
    Status unmask(interrupt::IrqLine line)
    {
        return apic_ ? io_apic_.unmask(line) : pic_.unmask(line);
    }
    // The 8259 pair delivers to the boot CPU only.
    Status route(interrupt::IrqLine line, ok::smp::CpuId cpu)
    {
        if (apic_)
            return io_apic_.route(line, cpu);
        return cpu == 0 ? Status::success() : Status::unsupported("the 8259 pair delivers to the boot CPU only");
    }
    Status acknowledge(interrupt::IrqLine line)
    {
        return apic_ ? io_apic_.acknowledge(line) : pic_.acknowledge(line);
    }
    Status end_of_interrupt(interrupt::IrqLine line)
    {
        return apic_ ? io_apic_.end_of_interrupt(line) : pic_.end_of_interrupt(line);
    }
    [[nodiscard]] usize line_count() const
    {
        return apic_ ? io_apic_.line_count() : pic_.line_count();
    }
    [[nodiscard]] std::string_view name() const
    {
        return apic_ ? io_apic_.name() : pic_.name();
    }
    [[nodiscard]] bool loses_masked_edges() const
    {
        return apic_;
    }

  private:
    IoApic io_apic_{};
    interrupt::Pic8259<interrupt::X86PortIo> pic_{};
    bool apic_{false};
};
static_assert(interrupt::RoutableIrqChip<PcIrqChip>);

} // namespace ok::arch::x86
